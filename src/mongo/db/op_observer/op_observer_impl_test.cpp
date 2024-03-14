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

// IWYU pragma: no_include "ext/alloc_traits.h"
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <limits>
#include <ostream>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/util/builder.h"
#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection_options_gen.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/import_collection_oplog_entry_gen.h"
#include "mongo/db/catalog/local_oplog_info.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer/batched_write_context.h"
#include "mongo/db/op_observer/change_stream_pre_images_op_observer.h"
#include "mongo/db/op_observer/find_and_modify_images_op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/op_observer/operation_logger_impl.h"
#include "mongo/db/pipeline/change_stream_preimage_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/read_write_concern_defaults_gen.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/apply_ops_command_info.h"
#include "mongo/db/repl/image_collection_entry_gen.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/repl/tenant_migration_access_blocker_registry.h"
#include "mongo/db/repl/tenant_migration_donor_access_blocker.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/session/session.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction/transaction_participant_gen.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/duration.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using repl::OplogEntry;
using unittest::assertGet;

OplogEntry getInnerEntryFromApplyOpsOplogEntry(const OplogEntry& oplogEntry) {
    std::vector<repl::OplogEntry> innerEntries;
    ASSERT(oplogEntry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
    repl::ApplyOps::extractOperationsTo(oplogEntry, oplogEntry.getEntry().toBSON(), &innerEntries);
    ASSERT_EQ(innerEntries.size(), 1u);
    return innerEntries[0];
}

void beginRetryableWriteWithTxnNumber(
    OperationContext* opCtx,
    TxnNumber txnNumber,
    std::unique_ptr<MongoDSessionCatalog::Session>& contextSession) {
    opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx->setTxnNumber(txnNumber);

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    contextSession = mongoDSessionCatalog->checkOutSession(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);
    txnParticipant.beginOrContinue(opCtx,
                                   {*opCtx->getTxnNumber()},
                                   boost::none /* autocommit */,
                                   TransactionParticipant::TransactionActions::kNone);
}

void beginNonRetryableTransactionWithTxnNumber(
    OperationContext* opCtx,
    TxnNumber txnNumber,
    std::unique_ptr<MongoDSessionCatalog::Session>& contextSession) {
    opCtx->setLogicalSessionId(makeLogicalSessionIdForTest());
    opCtx->setTxnNumber(txnNumber);
    opCtx->setInMultiDocumentTransaction();

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    contextSession = mongoDSessionCatalog->checkOutSession(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);
    txnParticipant.beginOrContinue(opCtx,
                                   {*opCtx->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
}

void beginRetryableInternalTransactionWithTxnNumber(
    OperationContext* opCtx,
    TxnNumber txnNumber,
    std::unique_ptr<MongoDSessionCatalog::Session>& contextSession) {
    opCtx->setLogicalSessionId(makeLogicalSessionIdWithTxnNumberAndUUIDForTest());
    opCtx->setTxnNumber(txnNumber);
    opCtx->setInMultiDocumentTransaction();

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx);
    contextSession = mongoDSessionCatalog->checkOutSession(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);
    txnParticipant.beginOrContinue(opCtx,
                                   {*opCtx->getTxnNumber()},
                                   false /* autocommit */,
                                   TransactionParticipant::TransactionActions::kStart);
}

template <typename OpObserverType>
void commitUnpreparedTransaction(OperationContext* opCtx, OpObserverType& opObserver) {
    auto txnParticipant = TransactionParticipant::get(opCtx);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx);
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(),
                  txnParticipant.getNumberOfPrePostImagesToWriteForTest());

    // OpObserverImpl no longer reserves optimes when committing unprepared transactions.
    // This is now done in TransactionParticipant::Participant::commitUnpreparedTransaction()
    // prior to notifying the OpObserver. For test coverage specific to empty unprepared
    // multi-document transactions, see TxnParticipantTest::EmptyUnpreparedTransactionCommit.
    std::vector<OplogSlot> reservedSlots;
    if (!txnOps->isEmpty()) {
        reservedSlots = repl::getNextOpTimes(
            opCtx, txnOps->numOperations() + txnOps->getNumberOfPrePostImagesToWrite());
    }

    auto applyOpsOplogSlotAndOperationAssignment =
        txnOps->getApplyOpsInfo(reservedSlots,
                                getMaxNumberOfTransactionOperationsInSingleOplogEntry(),
                                getMaxSizeOfTransactionOperationsInSingleOplogEntryBytes(),
                                /*prepare=*/false);
    opObserver.onUnpreparedTransactionCommit(
        opCtx, reservedSlots, *txnOps, applyOpsOplogSlotAndOperationAssignment);
}

std::vector<repl::OpTime> reserveOpTimesInSideTransaction(OperationContext* opCtx, size_t count) {
    TransactionParticipant::SideTransactionBlock sideTxn{opCtx};

    WriteUnitOfWork wuow{opCtx};
    auto reservedSlots = LocalOplogInfo::get(opCtx)->getNextOpTimes(opCtx, count);
    wuow.release();

    shard_role_details::getRecoveryUnit(opCtx)->abortUnitOfWork();
    shard_role_details::getLocker(opCtx)->endWriteUnitOfWork();

    return reservedSlots;
}

repl::OpTime reserveOpTimeInSideTransaction(OperationContext* opCtx) {
    return reserveOpTimesInSideTransaction(opCtx, 1)[0];
}

class OpObserverTest : public ServiceContextMongoDTest {
protected:
    explicit OpObserverTest(Options options = {}) : ServiceContextMongoDTest(std::move(options)) {}

    virtual void setUp() override {
        // Set up mongod.
        ServiceContextMongoDTest::setUp();

        auto service = getServiceContext();
        auto opCtx = cc().makeOperationContext();
        // onStepUp() relies on the storage interface to create the config.transactions table.
        repl::StorageInterface::set(service, std::make_unique<repl::StorageInterfaceImpl>());

        // Set up ReplicationCoordinator and create oplog.
        repl::ReplicationCoordinator::set(
            service,
            std::make_unique<repl::ReplicationCoordinatorMock>(service, createReplSettings()));
        repl::createOplog(opCtx.get());

        // Ensure that we are primary.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));

        MongoDSessionCatalog::set(
            opCtx->getServiceContext(),
            std::make_unique<MongoDSessionCatalog>(
                std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));
        auto mongoDSessionCatalog = MongoDSessionCatalog::get(opCtx.get());
        mongoDSessionCatalog->onStepUp(opCtx.get());

        ReadWriteConcernDefaults::create(getServiceContext(), _lookupMock.getFetchDefaultsFn());

        reset(opCtx.get(), nss, uuid);
        reset(opCtx.get(), nss1, uuid1);
        reset(opCtx.get(), nss2, uuid2);
        reset(opCtx.get(), nss3, uuid3);
        reset(opCtx.get(), kNssUnderTenantId, kNssUnderTenantIdUUID);
        reset(opCtx.get(), NamespaceString::kRsOplogNamespace);
    }

    void tearDown() override {
        serverGlobalParams.clusterRole = ClusterRole::None;
    }

    void reset(OperationContext* opCtx,
               NamespaceString nss,
               boost::optional<UUID> uuid = boost::none) const {
        writeConflictRetry(opCtx, "deleteAll", nss, [&] {
            shard_role_details::getRecoveryUnit(opCtx)->setTimestampReadSource(
                RecoveryUnit::ReadSource::kNoTimestamp);
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

            WriteUnitOfWork wunit(opCtx);
            AutoGetCollection collRaii(opCtx, nss, MODE_X);
            if (collRaii) {
                invariant(collRaii.getWritableCollection(opCtx)->truncate(opCtx).isOK());
            } else {
                auto db = collRaii.ensureDbExists(opCtx);
                CollectionOptions opts;
                if (uuid) {
                    opts.uuid = uuid;
                }
                invariant(db->createCollection(opCtx, nss, opts));
            }
            wunit.commit();
        });
    }

    void resetOplogAndTransactions(OperationContext* opCtx) const {
        reset(opCtx, nss);
        reset(opCtx, NamespaceString::kRsOplogNamespace);
        reset(opCtx, NamespaceString::kSessionTransactionsTableNamespace);
        reset(opCtx, NamespaceString::kConfigImagesNamespace);
        reset(opCtx, NamespaceString::makePreImageCollectionNSS(boost::none));
    }

    // Assert that the oplog has the expected number of entries, and return them
    std::vector<BSONObj> getNOplogEntries(OperationContext* opCtx, int numExpected) {
        std::vector<BSONObj> allOplogEntries;
        repl::OplogInterfaceLocal oplogInterface(opCtx);

        AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(
            shard_role_details::getLocker(opCtx));
        auto oplogIter = oplogInterface.makeIterator();
        while (true) {
            StatusWith<std::pair<BSONObj, RecordId>> swEntry = oplogIter->next();
            if (swEntry.getStatus() == ErrorCodes::CollectionIsEmpty) {
                break;
            }
            allOplogEntries.push_back(swEntry.getValue().first);
        }
        if (allOplogEntries.size() != static_cast<std::size_t>(numExpected)) {
            LOGV2(5739903,
                  "Incorrect number of oplog entries made",
                  "numExpected"_attr = numExpected,
                  "numFound"_attr = allOplogEntries.size(),
                  "entries"_attr = allOplogEntries);
        }
        ASSERT_EQUALS(allOplogEntries.size(), numExpected);

        std::vector<BSONObj> ret(numExpected);
        for (int idx = numExpected - 1; idx >= 0; idx--) {
            // The oplogIterator returns the entries in reverse order.
            ret[idx] = allOplogEntries[numExpected - idx - 1];
        }

        // Some unittests reuse the same OperationContext to read the oplog and end up acquiring the
        // RSTL lock after using the OplogInterfaceLocal. This is a hack to make sure we do not hold
        // RSTL lock for prepared transactions.
        if (opCtx->inMultiDocumentTransaction() &&
            TransactionParticipant::get(opCtx).transactionIsPrepared()) {
            shard_role_details::getLocker(opCtx)->unlockRSTLforPrepare();
        }
        return ret;
    }

    // Assert that oplog only has a single entry and return that oplog entry.
    BSONObj getSingleOplogEntry(OperationContext* opCtx) {
        return getNOplogEntries(opCtx, 1).back();
    }

    BSONObj getInnerEntryFromSingleApplyOpsOplogEntry(OperationContext* opCtx) {
        auto applyOpsOplogEntry = assertGet(OplogEntry::parse(getNOplogEntries(opCtx, 1).back()));
        return getInnerEntryFromApplyOpsOplogEntry(applyOpsOplogEntry).getEntry().toBSON();
    }

    bool didWriteImageEntryToSideCollection(OperationContext* opCtx,
                                            const LogicalSessionId& sessionId) {
        AutoGetCollection sideCollection(opCtx, NamespaceString::kConfigImagesNamespace, MODE_IS);
        const auto imageEntry = Helpers::findOneForTesting(opCtx,
                                                           sideCollection.getCollection(),
                                                           BSON("_id" << sessionId.toBSON()),
                                                           /*invariantOnError=*/false);
        return !imageEntry.isEmpty();
    }

    bool didWriteDeletedDocToPreImagesCollection(OperationContext* opCtx,
                                                 const ChangeStreamPreImageId preImageId) {
        AutoGetCollection preImagesCollection(
            opCtx, NamespaceString::makePreImageCollectionNSS(boost::none), LockMode::MODE_IS);
        const auto preImage = Helpers::findOneForTesting(opCtx,
                                                         preImagesCollection.getCollection(),
                                                         BSON("_id" << preImageId.toBSON()),
                                                         /*invariantOnError=*/false);
        return !preImage.isEmpty();
    }

    repl::ImageEntry getImageEntryFromSideCollection(OperationContext* opCtx,
                                                     const LogicalSessionId& sessionId) {
        AutoGetCollection sideCollection(opCtx, NamespaceString::kConfigImagesNamespace, MODE_IS);
        auto doc = Helpers::findOneForTesting(opCtx,
                                              sideCollection.getCollection(),
                                              BSON("_id" << sessionId.toBSON()),
                                              /*invariantOnError=*/false);
        ASSERT_FALSE(doc.isEmpty())
            << "Change stream pre-image not found: " << sessionId.toBSON()
            << " (pre-images collection: " << sideCollection->ns().toStringForErrorMsg() << ")";
        return repl::ImageEntry::parse(IDLParserContext("image entry"), doc);
    }

    SessionTxnRecord getTxnRecord(OperationContext* opCtx, const LogicalSessionId& sessionId) {
        AutoGetCollection configTransactions(
            opCtx, NamespaceString::kSessionTransactionsTableNamespace, MODE_IS);

        auto doc = Helpers::findOneForTesting(opCtx,
                                              configTransactions.getCollection(),
                                              BSON("_id" << sessionId.toBSON()),
                                              /*invariantOnError=*/false);
        ASSERT_FALSE(doc.isEmpty())
            << "Transaction not found for session: " << sessionId.toBSON()
            << "(transactions collection: " << configTransactions->ns().toStringForErrorMsg()
            << ")";
        return SessionTxnRecord::parse(IDLParserContext("txn record"), doc);
    }

    /**
     * The caller must pass a BSONObj container to own the preImage BSONObj part of a
     * `ChangeStreamPreImage`. The `ChangeStreamPreImage` idl declares the preImage BSONObj to not
     * be owned. Thus the BSONObject returned from the collection must outlive any accesses to
     * `ChangeStreamPreImage.getPreImage`.
     */
    ChangeStreamPreImage getChangeStreamPreImage(OperationContext* opCtx,
                                                 const ChangeStreamPreImageId& preImageId,
                                                 BSONObj* container) {
        AutoGetCollection preImagesCollection(
            opCtx, NamespaceString::makePreImageCollectionNSS(boost::none), LockMode::MODE_IS);
        *container = Helpers::findOneForTesting(opCtx,
                                                preImagesCollection.getCollection(),
                                                BSON("_id" << preImageId.toBSON()),
                                                /*invariantOnError=*/false);
        ASSERT_FALSE(container->isEmpty())
            << "Change stream pre-image not found: " << preImageId.toBSON()
            << " (pre-images collection: " << preImagesCollection->ns().toStringForErrorMsg()
            << ")";
        return ChangeStreamPreImage::parse(IDLParserContext("pre-image"), *container);
    }

    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(TenantId(OID::gen()), "testDB", "testColl");
    const UUID uuid{UUID::gen()};
    const NamespaceString nss1 = NamespaceString::createNamespaceString_forTest(
        TenantId(OID::gen()), "testDB1", "testColl1");
    const UUID uuid1{UUID::gen()};
    const NamespaceString nss2 = NamespaceString::createNamespaceString_forTest(
        TenantId(OID::gen()), "testDB2", "testColl2");
    const UUID uuid2{UUID::gen()};
    const NamespaceString nss3 =
        NamespaceString::createNamespaceString_forTest(boost::none, "testDB3", "testColl3");
    const UUID uuid3{UUID::gen()};

    const TenantId kTenantId = TenantId(OID::gen());
    const NamespaceString kNssUnderTenantId = NamespaceString::createNamespaceString_forTest(
        boost::none, kTenantId.toString() + "_db", "testColl");
    const UUID kNssUnderTenantIdUUID{UUID::gen()};

    ReadWriteConcernDefaultsLookupMock _lookupMock;

private:
    // Creates a reasonable set of ReplSettings for most tests.  We need to be able to
    // override this to create a larger oplog.
    virtual repl::ReplSettings createReplSettings() {
        repl::ReplSettings settings;
        settings.setOplogSizeBytes(5 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

TEST_F(OpObserverTest, StartIndexBuildExpectedOplogEntry) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    auto uuid = UUID::gen();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(boost::none, "test.coll");
    UUID indexBuildUUID = UUID::gen();

    BSONObj specX = BSON("key" << BSON("x" << 1) << "name"
                               << "x_1"
                               << "v" << 2);
    BSONObj specA = BSON("key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "v" << 2);
    std::vector<BSONObj> specs = {specX, specA};

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onStartIndexBuild(
            opCtx.get(), nss, uuid, indexBuildUUID, specs, false /*fromMigrate*/);
        wunit.commit();
    }

    // Create expected startIndexBuild command.
    BSONObjBuilder startIndexBuildBuilder;
    startIndexBuildBuilder.append("startIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&startIndexBuildBuilder, "indexBuildUUID");
    BSONArrayBuilder indexesArr(startIndexBuildBuilder.subarrayStart("indexes"));
    indexesArr.append(specX);
    indexesArr.append(specA);
    indexesArr.done();
    BSONObj startIndexBuildCmd = startIndexBuildBuilder.done();

    // Ensure the startIndexBuild fields were correctly set.
    auto oplogEntry = getSingleOplogEntry(opCtx.get());
    auto o = oplogEntry.getObjectField("o");
    ASSERT_BSONOBJ_EQ(startIndexBuildCmd, o);
}

TEST_F(OpObserverTest, CommitIndexBuildExpectedOplogEntry) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    auto uuid = UUID::gen();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(boost::none, "test.coll");
    UUID indexBuildUUID = UUID::gen();

    BSONObj specX = BSON("key" << BSON("x" << 1) << "name"
                               << "x_1"
                               << "v" << 2);
    BSONObj specA = BSON("key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "v" << 2);
    std::vector<BSONObj> specs = {specX, specA};

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCommitIndexBuild(
            opCtx.get(), nss, uuid, indexBuildUUID, specs, false /*fromMigrate*/);
        wunit.commit();
    }

    // Create expected commitIndexBuild command.
    BSONObjBuilder commitIndexBuildBuilder;
    commitIndexBuildBuilder.append("commitIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&commitIndexBuildBuilder, "indexBuildUUID");
    BSONArrayBuilder indexesArr(commitIndexBuildBuilder.subarrayStart("indexes"));
    indexesArr.append(specX);
    indexesArr.append(specA);
    indexesArr.done();
    BSONObj commitIndexBuildCmd = commitIndexBuildBuilder.done();

    // Ensure the commitIndexBuild fields were correctly set.
    auto oplogEntry = getSingleOplogEntry(opCtx.get());
    auto o = oplogEntry.getObjectField("o");
    ASSERT_BSONOBJ_EQ(commitIndexBuildCmd, o);
}

TEST_F(OpObserverTest, AbortIndexBuildExpectedOplogEntry) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    auto uuid = UUID::gen();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(boost::none, "test.coll");
    UUID indexBuildUUID = UUID::gen();

    BSONObj specX = BSON("key" << BSON("x" << 1) << "name"
                               << "x_1"
                               << "v" << 2);
    BSONObj specA = BSON("key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "v" << 2);
    std::vector<BSONObj> specs = {specX, specA};

    // Write to the oplog.
    Status cause(ErrorCodes::OperationFailed, "index build failed");
    {
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        auto fromMigrate = false;
        opObserver.onAbortIndexBuild(
            opCtx.get(), nss, uuid, indexBuildUUID, specs, cause, fromMigrate);
        wunit.commit();
    }

    // Create expected abortIndexBuild command.
    BSONObjBuilder abortIndexBuildBuilder;
    abortIndexBuildBuilder.append("abortIndexBuild", nss.coll());
    indexBuildUUID.appendToBuilder(&abortIndexBuildBuilder, "indexBuildUUID");
    BSONArrayBuilder indexesArr(abortIndexBuildBuilder.subarrayStart("indexes"));
    indexesArr.append(specX);
    indexesArr.append(specA);
    indexesArr.done();
    BSONObjBuilder causeBuilder(abortIndexBuildBuilder.subobjStart("cause"));
    causeBuilder.appendBool("ok", 0);
    cause.serializeErrorToBSON(&causeBuilder);
    causeBuilder.done();
    BSONObj abortIndexBuildCmd = abortIndexBuildBuilder.done();

    // Ensure the abortIndexBuild fields were correctly set.
    auto oplogEntry = getSingleOplogEntry(opCtx.get());
    auto o = oplogEntry.getObjectField("o");
    ASSERT_BSONOBJ_EQ(abortIndexBuildCmd, o);

    // Should be able to extract a Status from the 'cause' field.
    ASSERT_EQUALS(cause, getStatusFromCommandResult(o.getObjectField("cause")));
}

TEST_F(OpObserverTest, CollModWithCollectionOptionsAndTTLInfo) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    auto uuid = UUID::gen();

    // Create 'collMod' command.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(boost::none, "test.coll");
    BSONObj collModCmd = BSON("collMod" << nss.coll() << "validationLevel"
                                        << "off"
                                        << "validationAction"
                                        << "warn"
                                        // We verify that 'onCollMod' ignores this field.
                                        << "index"
                                        << "indexData");

    CollectionOptions oldCollOpts;
    oldCollOpts.validationLevel = ValidationLevelEnum::strict;
    oldCollOpts.validationAction = ValidationActionEnum::error;

    IndexCollModInfo indexInfo;
    indexInfo.expireAfterSeconds = Seconds(10);
    indexInfo.oldExpireAfterSeconds = Seconds(5);
    indexInfo.indexName = "name_of_index";

    // Write to the oplog.
    {
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCollMod(opCtx.get(), nss, uuid, collModCmd, oldCollOpts, indexInfo);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that collMod fields were properly added to the oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON(
        "collMod" << nss.coll() << "validationLevel"
                  << "off"
                  << "validationAction"
                  << "warn"
                  << "index"
                  << BSON("name" << indexInfo.indexName << "expireAfterSeconds"
                                 << durationCount<Seconds>(indexInfo.expireAfterSeconds.value())));
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the old collection metadata was saved.
    auto o2 = oplogEntry.getObjectField("o2");
    auto o2Expected = BSON("collectionOptions_old"
                           << BSON("validationLevel"
                                   << ValidationLevel_serializer(*oldCollOpts.validationLevel)
                                   << "validationAction"
                                   << ValidationAction_serializer(*oldCollOpts.validationAction))
                           << "indexOptions_old"
                           << BSON("expireAfterSeconds" << durationCount<Seconds>(
                                       indexInfo.oldExpireAfterSeconds.value())));

    ASSERT_BSONOBJ_EQ(o2Expected, o2);
}

TEST_F(OpObserverTest, CollModWithOnlyCollectionOptions) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    auto uuid = UUID::gen();

    // Create 'collMod' command.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(boost::none, "test.coll");
    BSONObj collModCmd = BSON("collMod" << nss.coll() << "validationLevel"
                                        << "off"
                                        << "validationAction"
                                        << "warn");

    CollectionOptions oldCollOpts;
    oldCollOpts.validationLevel = ValidationLevelEnum::strict;
    oldCollOpts.validationAction = ValidationActionEnum::error;

    // Write to the oplog.
    {
        AutoGetCollection autoColl(opCtx.get(), nss, MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onCollMod(opCtx.get(), nss, uuid, collModCmd, oldCollOpts, boost::none);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that collMod fields were properly added to oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = collModCmd;
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the old collection metadata was saved and that TTL info is not present.
    auto o2 = oplogEntry.getObjectField("o2");
    auto o2Expected = BSON("collectionOptions_old"
                           << BSON("validationLevel"
                                   << ValidationLevel_serializer(*oldCollOpts.validationLevel)
                                   << "validationAction"
                                   << ValidationAction_serializer(*oldCollOpts.validationAction)));

    ASSERT_BSONOBJ_EQ(o2Expected, o2);
}

TEST_F(OpObserverTest, OnUpdateCheckExistenceForDiffInsert) {
    const auto criteria = BSON("_id" << 0);
    // Create a fake preImageDoc; the tested code path does not care about this value.
    const auto preImageDoc = criteria;
    CollectionUpdateArgs updateArgs{preImageDoc};
    updateArgs.criteria = criteria;
    updateArgs.updatedDoc = BSON("_id" << 0 << "data"
                                       << "x");
    updateArgs.update = BSON("$set" << BSON("data"
                                            << "x"));
    updateArgs.mustCheckExistenceForInsertOperations = true;

    auto opCtx = cc().makeOperationContext();
    WriteUnitOfWork wuow(opCtx.get());
    AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_X);
    AutoGetCollection autoColl(opCtx.get(), nss, MODE_X);
    OplogUpdateEntryArgs update(&updateArgs, *autoColl);

    OpObserverRegistry opObserver;
    opObserver.addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    opObserver.onUpdate(opCtx.get(), update);
    wuow.commit();

    auto oplogEntryObj = getSingleOplogEntry(opCtx.get());
    const repl::OplogEntry& entry = assertGet(repl::OplogEntry::parse(oplogEntryObj));

    ASSERT_TRUE(entry.getCheckExistenceForDiffInsert());
}

TEST_F(OpObserverTest, OnDropCollectionReturnsDropOpTime) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    auto uuid = UUID::gen();

    // Create 'drop' command.
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(boost::none, "test.coll");
    auto dropCmd = BSON("drop" << nss.coll());

    // Write to the oplog.
    repl::OpTime dropOpTime;
    {
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onDropCollection(opCtx.get(),
                                    nss,
                                    uuid,
                                    0U,
                                    OpObserver::CollectionDropType::kTwoPhase,
                                    /*markFromMigrate=*/false);
        dropOpTime = OpObserver::Times::get(opCtx.get()).reservedOpTimes.front();
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that drop fields were properly added to oplog entry.
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = dropCmd;
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the drop optime returned is the same as the last optime in the ReplClientInfo.
    ASSERT_EQUALS(repl::ReplClientInfo::forClient(&cc()).getLastOp(), dropOpTime);
}

TEST_F(OpObserverTest, OnDropCollectionInlcudesTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    auto uuid = UUID::gen();

    // Create 'drop' command.
    TenantId tid{TenantId(OID::gen())};
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "test.coll");
    auto dropCmd = BSON("drop" << nss.coll());

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onDropCollection(opCtx.get(),
                                    nss,
                                    uuid,
                                    0U,
                                    OpObserver::CollectionDropType::kTwoPhase,
                                    /*markFromMigrate=*/false);
        wunit.commit();
    }

    OplogEntry oplogEntry = assertGet(OplogEntry::parse(getSingleOplogEntry(opCtx.get())));
    ASSERT_EQUALS(tid, *oplogEntry.getTid());
    ASSERT_EQUALS(nss.getCommandNS(), oplogEntry.getNss());
}

TEST_F(OpObserverTest, OnRenameCollectionReturnsRenameOpTime) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();

    auto uuid = UUID::gen();
    auto dropTargetUuid = UUID::gen();
    auto stayTemp = false;
    NamespaceString sourceNss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test.foo");
    NamespaceString targetNss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test.bar");

    // Write to the oplog.
    repl::OpTime renameOpTime;
    {
        AutoGetDb autoDb(opCtx.get(), sourceNss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onRenameCollection(opCtx.get(),
                                      sourceNss,
                                      targetNss,
                                      uuid,
                                      dropTargetUuid,
                                      0U,
                                      stayTemp,
                                      /*markFromMigrate=*/false);
        renameOpTime = OpObserver::Times::get(opCtx.get()).reservedOpTimes.front();
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that renameCollection fields were properly added to oplog entry.
    ASSERT_EQUALS(uuid, unittest::assertGet(UUID::parse(oplogEntry["ui"])));
    auto o = oplogEntry.getObjectField("o");
    auto oExpected =
        BSON("renameCollection" << sourceNss.ns_forTest() << "to" << targetNss.ns_forTest()
                                << "stayTemp" << stayTemp << "dropTarget" << dropTargetUuid);
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // Ensure that the rename optime returned is the same as the last optime in the ReplClientInfo.
    ASSERT_EQUALS(repl::ReplClientInfo::forClient(&cc()).getLastOp(), renameOpTime);
}

TEST_F(OpObserverTest, OnRenameCollectionIncludesTenantIdFeatureFlagOff) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", false);
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();

    auto uuid = UUID::gen();
    auto dropTargetUuid = UUID::gen();
    auto stayTemp = false;
    auto tid{TenantId(OID::gen())};  // rename should not occur across tenants
    NamespaceString sourceNss = NamespaceString::createNamespaceString_forTest(tid, "test.foo");
    NamespaceString targetNss = NamespaceString::createNamespaceString_forTest(tid, "test.bar");

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), sourceNss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onRenameCollection(opCtx.get(),
                                      sourceNss,
                                      targetNss,
                                      uuid,
                                      dropTargetUuid,
                                      0U,
                                      stayTemp,
                                      /*markFromMigrate=*/false);
        wunit.commit();
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx.get());
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));

    // Ensure that renameCollection fields were properly added to oplog entry.
    ASSERT_EQUALS(uuid, unittest::assertGet(UUID::parse(oplogEntryObj["ui"])));
    ASSERT_FALSE(oplogEntry.getTid());
    ASSERT_EQUALS(sourceNss.getCommandNS(), oplogEntry.getNss());

    auto oExpected =
        BSON("renameCollection" << sourceNss.toStringWithTenantId_forTest() << "to"
                                << targetNss.toStringWithTenantId_forTest() << "stayTemp"
                                << stayTemp << "dropTarget" << dropTargetUuid);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntry.getObject());
}

TEST_F(OpObserverTest, OnRenameCollectionIncludesTenantIdFeatureFlagOn) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();

    auto uuid = UUID::gen();
    auto dropTargetUuid = UUID::gen();
    auto stayTemp = false;
    auto tid{TenantId(OID::gen())};  // rename should not occur across tenants
    NamespaceString sourceNss = NamespaceString::createNamespaceString_forTest(tid, "test.foo");
    NamespaceString targetNss = NamespaceString::createNamespaceString_forTest(tid, "test.bar");

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), sourceNss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onRenameCollection(opCtx.get(),
                                      sourceNss,
                                      targetNss,
                                      uuid,
                                      dropTargetUuid,
                                      0U,
                                      stayTemp,
                                      /*markFromMigrate=*/false);
        wunit.commit();
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx.get());
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));

    // Ensure that renameCollection fields were properly added to oplog entry.
    ASSERT_EQUALS(uuid, unittest::assertGet(UUID::parse(oplogEntryObj["ui"])));
    ASSERT_EQUALS(tid, *oplogEntry.getTid());
    ASSERT_EQUALS(sourceNss.getCommandNS(), oplogEntry.getNss());

    auto oExpected = BSON("renameCollection" << sourceNss.toString_forTest() << "to"
                                             << targetNss.toString_forTest() << "stayTemp"
                                             << stayTemp << "dropTarget" << dropTargetUuid);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntry.getObject());
}

TEST_F(OpObserverTest, OnRenameCollectionOmitsDropTargetFieldIfDropTargetUuidIsNull) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();

    auto uuid = UUID::gen();
    auto stayTemp = true;
    NamespaceString sourceNss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test.foo");
    NamespaceString targetNss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test.bar");

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), sourceNss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onRenameCollection(
            opCtx.get(), sourceNss, targetNss, uuid, {}, 0U, stayTemp, /*markFromMigrate=*/false);
        wunit.commit();
    }

    auto oplogEntry = getSingleOplogEntry(opCtx.get());

    // Ensure that renameCollection fields were properly added to oplog entry.
    ASSERT_EQUALS(uuid, unittest::assertGet(UUID::parse(oplogEntry["ui"])));
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON("renameCollection" << sourceNss.ns_forTest() << "to"
                                             << targetNss.ns_forTest() << "stayTemp" << stayTemp);
    ASSERT_BSONOBJ_EQ(oExpected, o);
}

TEST_F(OpObserverTest, MustBePrimaryToWriteOplogEntries) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();

    ASSERT_OK(repl::ReplicationCoordinator::get(opCtx.get())
                  ->setFollowerMode(repl::MemberState::RS_SECONDARY));

    Lock::GlobalWrite globalWrite(opCtx.get());
    WriteUnitOfWork wunit(opCtx.get());

    // No-op writes should be prohibited.
    ASSERT_THROWS_CODE(
        opObserver.onOpMessage(opCtx.get(), {}), DBException, ErrorCodes::NotWritablePrimary);
}

TEST_F(OpObserverTest, ImportCollectionOplogEntry) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();

    auto importUUID = UUID::gen();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(boost::none, "test.coll");
    long long numRecords = 1;
    long long dataSize = 2;
    // A dummy invalid catalog entry. We do not need a valid catalog entry for this test.
    auto catalogEntry = BSON("ns" << nss.ns_forTest() << "ident"
                                  << "collection-7-1792004489479993697");
    auto storageMetadata = BSON("storage"
                                << "metadata");
    bool isDryRun = false;

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onImportCollection(opCtx.get(),
                                      importUUID,
                                      nss,
                                      numRecords,
                                      dataSize,
                                      catalogEntry,
                                      storageMetadata,
                                      isDryRun);
        wunit.commit();
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx.get());
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    ASSERT_TRUE(repl::OpTypeEnum::kCommand == oplogEntry.getOpType());
    ASSERT_TRUE(OplogEntry::CommandType::kImportCollection == oplogEntry.getCommandType());

    ImportCollectionOplogEntry importCollection(
        nss, importUUID, numRecords, dataSize, catalogEntry, storageMetadata, isDryRun);
    ASSERT_BSONOBJ_EQ(importCollection.toBSON(), oplogEntry.getObject());
}

TEST_F(OpObserverTest, ImportCollectionOplogEntryIncludesTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();

    auto importUUID = UUID::gen();
    TenantId tid{TenantId(OID::gen())};
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(tid, "test.coll");
    long long numRecords = 1;
    long long dataSize = 2;
    // A dummy invalid catalog entry. We do not need a valid catalog entry for this test.
    auto catalogEntry = BSON("ns" << nss.ns_forTest() << "ident"
                                  << "collection-7-1792004489479993697");
    auto storageMetadata = BSON("storage"
                                << "metadata");
    bool isDryRun = false;

    // Write to the oplog.
    {
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onImportCollection(opCtx.get(),
                                      importUUID,
                                      nss,
                                      numRecords,
                                      dataSize,
                                      catalogEntry,
                                      storageMetadata,
                                      isDryRun);
        wunit.commit();
    }

    auto oplogEntryObj = getSingleOplogEntry(opCtx.get());
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    ASSERT_TRUE(repl::OpTypeEnum::kCommand == oplogEntry.getOpType());
    ASSERT_TRUE(OplogEntry::CommandType::kImportCollection == oplogEntry.getCommandType());

    ASSERT_EQUALS(tid, *oplogEntry.getTid());

    ImportCollectionOplogEntry importCollection(
        nss, importUUID, numRecords, dataSize, catalogEntry, storageMetadata, isDryRun);
    ASSERT_BSONOBJ_EQ(importCollection.toBSON(), oplogEntry.getObject());
}

TEST_F(OpObserverTest, SingleStatementInsertTestIncludesTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    std::vector<InsertStatement> insert;
    insert.emplace_back(BSON("_id" << 0 << "data"
                                   << "x"));

    auto opCtx = cc().makeOperationContext();
    WriteUnitOfWork wuow(opCtx.get());
    AutoGetCollection autoColl(opCtx.get(), nss, MODE_IX);

    OpObserverRegistry opObserver;
    opObserver.addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    opObserver.onInserts(opCtx.get(),
                         *autoColl,
                         insert.begin(),
                         insert.end(),
                         /*recordIds=*/{},
                         /*fromMigrate=*/std::vector<bool>(insert.size(), false),
                         /*defaultFromMigrate=*/false);
    wuow.commit();

    auto oplogEntryObj = getSingleOplogEntry(opCtx.get());
    const repl::OplogEntry& entry = assertGet(repl::OplogEntry::parse(oplogEntryObj));

    ASSERT_EQ(nss, entry.getNss());
    ASSERT(nss.tenantId().has_value());

    ASSERT_EQ(*nss.tenantId(), *entry.getTid());
    ASSERT_EQ(uuid, *entry.getUuid());
}

TEST_F(OpObserverTest, SingleStatementUpdateTestIncludesTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    const auto criteria = BSON("_id" << 0);
    // Create a fake preImageDoc; the tested code path does not care about this value.
    const auto preImageDoc = criteria;
    CollectionUpdateArgs updateArgs{preImageDoc};
    updateArgs.criteria = criteria;
    updateArgs.updatedDoc = BSON("_id" << 0 << "data"
                                       << "x");
    updateArgs.update = BSON("$set" << BSON("data"
                                            << "x"));

    auto opCtx = cc().makeOperationContext();
    WriteUnitOfWork wuow(opCtx.get());
    AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_X);
    AutoGetCollection autoColl(opCtx.get(), nss, MODE_X);
    OplogUpdateEntryArgs update(&updateArgs, *autoColl);

    OpObserverRegistry opObserver;
    opObserver.addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    opObserver.onUpdate(opCtx.get(), update);
    wuow.commit();

    auto oplogEntryObj = getSingleOplogEntry(opCtx.get());
    const repl::OplogEntry& entry = assertGet(repl::OplogEntry::parse(oplogEntryObj));

    ASSERT(nss.tenantId().has_value());
    ASSERT_EQ(*nss.tenantId(), *entry.getTid());
    ASSERT_EQ(nss, entry.getNss());
    ASSERT_EQ(uuid, *entry.getUuid());
}

TEST_F(OpObserverTest, SingleStatementDeleteTestIncludesTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    auto opCtx = cc().makeOperationContext();
    WriteUnitOfWork wuow(opCtx.get());
    AutoGetCollection locks(opCtx.get(), nss, MODE_IX);

    OpObserverRegistry opObserver;
    opObserver.addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    // This test does not call `OpObserver::aboutToDelete`. That method has the side-effect
    // of setting of `documentKey` on the delete for sharding purposes.
    // `OpObserverImpl::onDelete` asserts its existence.
    OplogDeleteEntryArgs deleteEntryArgs;
    auto doc = BSON("_id" << 0);
    documentKeyDecoration(deleteEntryArgs).emplace(doc, boost::none);
    opObserver.onDelete(opCtx.get(), *locks, kUninitializedStmtId, doc, deleteEntryArgs);
    wuow.commit();

    auto oplogEntryObj = getSingleOplogEntry(opCtx.get());
    const repl::OplogEntry& entry = assertGet(repl::OplogEntry::parse(oplogEntryObj));

    ASSERT(nss.tenantId().has_value());
    ASSERT_EQ(nss, entry.getNss());
    ASSERT_EQ(*nss.tenantId(), *entry.getTid());
    ASSERT_EQ(uuid, *entry.getUuid());
}

/**
 * Test fixture for testing OpObserver behavior specific to the SessionCatalog.
 */
class OpObserverSessionCatalogRollbackTest : public OpObserverTest {
public:
    void setUp() override {
        OpObserverTest::setUp();

        auto opCtx = cc().makeOperationContext();
    }

    /**
     * Simulate a new write occurring on given session with the given transaction number and
     * statement id.
     */
    void simulateSessionWrite(OperationContext* opCtx,
                              TransactionParticipant::Participant txnParticipant,
                              NamespaceString nss,
                              TxnNumber txnNum,
                              StmtId stmtId) {
        txnParticipant.beginOrContinue(opCtx,
                                       {txnNum},
                                       boost::none /* autocommit */,
                                       TransactionParticipant::TransactionActions::kNone);

        {
            AutoGetCollection autoColl(opCtx, nss, MODE_IX);
            WriteUnitOfWork wuow(opCtx);
            auto opTime = repl::OpTime(Timestamp(10, 1), 1);  // Dummy timestamp.
            SessionTxnRecord sessionTxnRecord;
            sessionTxnRecord.setSessionId(*opCtx->getLogicalSessionId());
            sessionTxnRecord.setTxnNum(txnNum);
            sessionTxnRecord.setLastWriteOpTime(opTime);
            sessionTxnRecord.setLastWriteDate(Date_t::now());
            txnParticipant.onWriteOpCompletedOnPrimary(opCtx, {stmtId}, sessionTxnRecord);
            wuow.commit();
        }
    }
};

TEST_F(OpObserverSessionCatalogRollbackTest,
       OnRollbackDoesntInvalidateSessionCatalogIfNoSessionOpsRolledBack) {
    const NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "testDB", "testColl");

    auto sessionId = makeLogicalSessionIdForTest();

    const TxnNumber txnNum = 0;
    const StmtId stmtId = 1000;

    auto mongoDSessionCatalog = MongoDSessionCatalog::get(getServiceContext());
    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(sessionId);
        opCtx->setTxnNumber(txnNum);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx.get());
        auto txnParticipant = TransactionParticipant::get(opCtx.get());
        txnParticipant.refreshFromStorageIfNeeded(opCtx.get());

        // Simulate a write occurring on that session
        simulateSessionWrite(opCtx.get(), txnParticipant, nss, txnNum, stmtId);

        // Check that the statement executed
        ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx.get(), stmtId));
    }

    // Because there are no sessions to rollback, the OpObserver should not invalidate the in-memory
    // session state, so the check after this should still succeed.
    {
        auto opCtx = cc().makeOperationContext();

        OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
        OpObserver::RollbackObserverInfo rbInfo;
        opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    }

    {
        auto opCtx = cc().makeOperationContext();
        opCtx->setLogicalSessionId(sessionId);
        auto ocs = mongoDSessionCatalog->checkOutSession(opCtx.get());
        auto txnParticipant = TransactionParticipant::get(opCtx.get());
        ASSERT(txnParticipant.checkStatementExecutedNoOplogEntryFetch(opCtx.get(), stmtId));
    }
}

TEST_F(OpObserverTest, MultipleAboutToDeleteAndOnDelete) {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    AutoGetCollection autoColl(opCtx.get(), nss3, MODE_X);
    WriteUnitOfWork wunit(opCtx.get());
    OplogDeleteEntryArgs args;
    auto doc = BSON("_id" << 1);
    opObserver.aboutToDelete(opCtx.get(), *autoColl, doc, &args);
    opObserver.onDelete(opCtx.get(), *autoColl, kUninitializedStmtId, doc, args);
    opObserver.aboutToDelete(opCtx.get(), *autoColl, doc, &args);
    opObserver.onDelete(opCtx.get(), *autoColl, kUninitializedStmtId, doc, args);
}

DEATH_TEST_REGEX_F(OpObserverTest,
                   AboutToDeleteMustPreceedOnDelete,
                   "Invariant failure.*optDocKey") {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    AutoGetCollection autoColl(opCtx.get(), nss3, MODE_IX);
    OplogDeleteEntryArgs args;
    auto doc = BSON("_id" << 1);
    opObserver.onDelete(opCtx.get(), *autoColl, kUninitializedStmtId, doc, args);
}

DEATH_TEST_REGEX_F(OpObserverTest,
                   AboutToDeleteRequiresIdField,
                   "Invariant failure.*!id.isEmpty()") {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    AutoGetCollection autoColl(opCtx.get(), nss3, MODE_IX);
    OplogDeleteEntryArgs args;
    opObserver.aboutToDelete(opCtx.get(), *autoColl, /*doc=*/{}, &args);
}

DEATH_TEST_REGEX_F(OpObserverTest,
                   NodeCrashesIfShardIdentityDocumentRolledBack,
                   "Fatal assertion.*50712") {
    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();

    OpObserver::RollbackObserverInfo rbInfo;
    rbInfo.shardIdentityRolledBack = true;
    opObserver.onReplicationRollback(opCtx.get(), rbInfo);
}

class OpObserverTxnParticipantTest : public OpObserverTest {
public:
    void tearDown() override {
        _sessionCheckout.reset();
        _times.reset();
        _opCtx.reset();

        OpObserverTest::tearDown();
    }

    void setUpObserverContext() {
        _opCtx = cc().makeOperationContext();
        _opObserver.emplace(std::make_unique<OperationLoggerImpl>());
        _times.emplace(opCtx());
    }

    void setUpRetryableWrite() {
        beginRetryableWriteWithTxnNumber(opCtx(), txnNum(), _sessionCheckout);
        _txnParticipant.emplace(TransactionParticipant::get(opCtx()));
    }

    void setUpNonRetryableTransaction() {
        beginNonRetryableTransactionWithTxnNumber(opCtx(), txnNum(), _sessionCheckout);
        _txnParticipant.emplace(TransactionParticipant::get(opCtx()));
    }

    void setUpRetryableInternalTransaction() {
        beginRetryableInternalTransactionWithTxnNumber(opCtx(), txnNum(), _sessionCheckout);
        _txnParticipant.emplace(TransactionParticipant::get(opCtx()));
    }

protected:
    Session* session() {
        return OperationContextSession::get(opCtx());
    }

    OpObserverImpl& opObserver() {
        return *_opObserver;
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    TxnNumber& txnNum() {
        return _txnNum;
    }

    TransactionParticipant::Participant& txnParticipant() {
        return *_txnParticipant;
    }

    void prepareTransaction(const std::vector<OplogSlot>& reservedSlots,
                            repl::OpTime prepareOpTime,
                            size_t numberOfPrePostImagesToWrite = 0) {
        auto txnOps = txnParticipant().retrieveCompletedTransactionOperations(opCtx());
        auto currentTime = Date_t::now();
        auto applyOpsAssignment =
            txnOps->getApplyOpsInfo(reservedSlots,
                                    getMaxNumberOfTransactionOperationsInSingleOplogEntry(),
                                    getMaxSizeOfTransactionOperationsInSingleOplogEntryBytes(),
                                    /*prepare=*/true);
        opObserver().preTransactionPrepare(opCtx(), *txnOps, applyOpsAssignment, currentTime);
        shard_role_details::getRecoveryUnit(opCtx())->setPrepareTimestamp(
            prepareOpTime.getTimestamp());

        // Don't write oplog entry on secondaries.
        if (opCtx()->writesAreReplicated()) {
            auto opCtxForPrepare = opCtx();
            TransactionParticipant::SideTransactionBlock sideTxn(opCtxForPrepare);

            writeConflictRetry(
                opCtxForPrepare, "onTransactionPrepare", NamespaceString::kRsOplogNamespace, [&] {
                    WriteUnitOfWork wuow(opCtxForPrepare);
                    opObserver().onTransactionPrepare(opCtxForPrepare,
                                                      reservedSlots,
                                                      *txnOps,
                                                      applyOpsAssignment,
                                                      numberOfPrePostImagesToWrite,
                                                      currentTime);
                    wuow.commit();
                });
        }

        opObserver().postTransactionPrepare(opCtx(), reservedSlots, *txnOps);
    }

private:
    class ExposeOpObserverTimes : public OpObserver {
    public:
        typedef OpObserver::ReservedTimes ReservedTimes;
    };

    ServiceContext::UniqueOperationContext _opCtx;

    boost::optional<OpObserverImpl> _opObserver;
    boost::optional<ExposeOpObserverTimes::ReservedTimes> _times;
    boost::optional<TransactionParticipant::Participant> _txnParticipant;

    std::unique_ptr<MongoDSessionCatalog::Session> _sessionCheckout;
    TxnNumber _txnNum = 0;
};

/**
 * Test fixture for testing OpObserver behavior specific to multi-document transactions.
 */

class OpObserverTransactionTest : public OpObserverTxnParticipantTest {
protected:
    void setUp() override {
        OpObserverTxnParticipantTest::setUp();
        setUpObserverContext();
        OpObserverTxnParticipantTest::setUpNonRetryableTransaction();
    }

    void checkSessionAndTransactionFields(const BSONObj& oplogEntry) {
        ASSERT_BSONOBJ_EQ(session()->getSessionId().toBSON(), oplogEntry.getObjectField("lsid"));
        ASSERT_EQ(*opCtx()->getTxnNumber(), oplogEntry.getField("txnNumber").safeNumberLong());
    }
    void checkCommonFields(const BSONObj& oplogEntry) {
        ASSERT_EQ("c"_sd, oplogEntry.getStringField("op"));
        ASSERT_EQ("admin.$cmd"_sd, oplogEntry.getStringField("ns"));
        checkSessionAndTransactionFields(oplogEntry);
    }

    void assertTxnRecord(TxnNumber txnNum,
                         repl::OpTime opTime,
                         boost::optional<DurableTxnStateEnum> txnState) {
        DBDirectClient client(opCtx());
        FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
        findRequest.setFilter(BSON("_id" << session()->getSessionId().toBSON()));
        auto cursor = client.find(std::move(findRequest));
        ASSERT(cursor);
        ASSERT(cursor->more());

        auto txnRecordObj = cursor->next();
        auto txnRecord =
            SessionTxnRecord::parse(IDLParserContext("SessionEntryWritten"), txnRecordObj);
        ASSERT(!cursor->more());
        ASSERT_EQ(session()->getSessionId(), txnRecord.getSessionId());
        ASSERT_EQ(txnNum, txnRecord.getTxnNum());
        ASSERT(txnRecord.getState() == txnState);
        ASSERT_EQ(txnState != boost::none,
                  txnRecordObj.hasField(SessionTxnRecord::kStateFieldName));

        auto txnParticipant = TransactionParticipant::get(opCtx());
        if (!opTime.isNull()) {
            ASSERT_EQ(opTime, txnRecord.getLastWriteOpTime());
            ASSERT_EQ(opTime, txnParticipant.getLastWriteOpTime());
        } else {
            ASSERT_EQ(txnRecord.getLastWriteOpTime(), txnParticipant.getLastWriteOpTime());
        }
    }

    void assertNoTxnRecord() {
        DBDirectClient client(opCtx());
        FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
        findRequest.setFilter(BSON("_id" << session()->getSessionId().toBSON()));
        auto cursor = client.find(std::move(findRequest));
        ASSERT(cursor);
        ASSERT(!cursor->more());
    }

    void assertTxnRecordStartOpTime(boost::optional<repl::OpTime> startOpTime) {
        DBDirectClient client(opCtx());
        FindCommandRequest findRequest{NamespaceString::kSessionTransactionsTableNamespace};
        findRequest.setFilter(BSON("_id" << session()->getSessionId().toBSON()));
        auto cursor = client.find(std::move(findRequest));
        ASSERT(cursor);
        ASSERT(cursor->more());

        auto txnRecordObj = cursor->next();
        auto txnRecord =
            SessionTxnRecord::parse(IDLParserContext("SessionEntryWritten"), txnRecordObj);
        ASSERT(!cursor->more());
        ASSERT_EQ(session()->getSessionId(), txnRecord.getSessionId());
        if (!startOpTime) {
            ASSERT(!txnRecord.getStartOpTime());
        } else {
            ASSERT(txnRecord.getStartOpTime());
            ASSERT_EQ(*startOpTime, *txnRecord.getStartOpTime());
        }
    }
};

TEST_F(OpObserverTransactionTest, TransactionalPrepareTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));
    opObserver().onInserts(opCtx(),
                           *autoColl1,
                           inserts1.begin(),
                           inserts1.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
                           /*defaultFromMigrate=*/false);

    const auto criteria = BSON("_id" << 0);
    // Create a fake preImageDoc; the tested code path does not care about this value.
    const auto preImageDoc = criteria;
    CollectionUpdateArgs updateArgs2{preImageDoc};
    updateArgs2.criteria = criteria;
    updateArgs2.stmtIds = {1};
    updateArgs2.updatedDoc = BSON("_id" << 0 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    OplogUpdateEntryArgs update2(&updateArgs2, *autoColl2);
    opObserver().onUpdate(opCtx(), update2);

    OplogDeleteEntryArgs args;
    auto docToDelete = BSON("_id" << 0 << "data"
                                  << "x");
    opObserver().aboutToDelete(opCtx(), *autoColl1, docToDelete, &args);
    opObserver().onDelete(opCtx(), *autoColl1, 0, docToDelete, args);

    // One reserved slot for each statement, plus the prepare.
    auto reservedSlots = reserveOpTimesInSideTransaction(opCtx(), 5);
    auto prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    prepareTransaction(reservedSlots, prepareOpTime);

    ASSERT_EQ(prepareOpTime.getTimestamp(),
              shard_role_details::getRecoveryUnit(opCtx())->getPrepareTimestamp());

    txnParticipant.stashTransactionResources(opCtx());
    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected = BSON(
        "applyOps" << BSON_ARRAY(BSON("op"
                                      << "i"
                                      << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                      << BSON("_id" << 0 << "data"
                                                    << "x")
                                      << "o2" << BSON("_id" << 0))
                                 << BSON("op"
                                         << "i"
                                         << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                         << BSON("_id" << 1 << "data"
                                                       << "y")
                                         << "o2" << BSON("_id" << 1))
                                 << BSON("op"
                                         << "u"
                                         << "ns" << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                         << BSON("$set" << BSON("data"
                                                                << "y"))
                                         << "o2" << BSON("_id" << 0))
                                 << BSON("op"
                                         << "d"
                                         << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                         << BSON("_id" << 0)))
                   << "prepare" << true);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(oplogEntry.shouldPrepare());
    ASSERT_EQ(oplogEntry.getTimestamp(), prepareOpTime.getTimestamp());

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverTransactionTest, TransactionalPreparedCommitTest) {
    const auto doc = BSON("_id" << 0 << "data"
                                << "x");
    const auto docKey = BSON("_id" << 0);

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0, doc);

    OplogSlot commitSlot;
    Timestamp prepareTimestamp;
    {
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(),
                               *autoColl,
                               insert.begin(),
                               insert.end(),
                               /*recordIds=*/{},
                               /*fromMigrate=*/std::vector<bool>(insert.size(), false),
                               /*defaultFromMigrate=*/false);

        const auto prepareSlot = reserveOpTimeInSideTransaction(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareSlot);
        prepareTimestamp = prepareSlot.getTimestamp();
        prepareTransaction({prepareSlot}, prepareSlot);

        commitSlot = reserveOpTimeInSideTransaction(opCtx());
    }

    // Mimic committing the transaction.
    shard_role_details::setWriteUnitOfWork(opCtx(), nullptr);
    shard_role_details::getLocker(opCtx())->unsetMaxLockTimeout();

    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onPreparedTransactionCommit(
            opCtx(),
            commitSlot,
            prepareTimestamp,
            txnParticipant.retrieveCompletedTransactionOperations(opCtx())
                ->getOperationsForOpObserver());
    }
    repl::OplogInterfaceLocal oplogInterface(opCtx());
    auto oplogIter = oplogInterface.makeIterator();
    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("commitTransaction" << 1 << "commitTimestamp" << prepareTimestamp);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT_FALSE(oplogEntry.shouldPrepare());
    }

    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected =
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << nss.toString_forTest() << "ui" << uuid
                                               << "o" << doc << "o2" << docKey))
                            << "prepare" << true);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT(oplogEntry.shouldPrepare());
    }

    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest, TransactionalPreparedAbortTest) {
    const auto doc = BSON("_id" << 0 << "data"
                                << "x");
    const auto docKey = BSON("_id" << 0);

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0, doc);

    OplogSlot abortSlot;
    {
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(),
                               *autoColl,
                               insert.begin(),
                               insert.end(),
                               /*recordIds=*/{},
                               /*fromMigrate=*/std::vector<bool>(insert.size(), false),
                               /*defaultFromMigrate=*/false);

        const auto prepareSlot = reserveOpTimeInSideTransaction(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareSlot);
        prepareTransaction({prepareSlot}, prepareSlot);
        abortSlot = reserveOpTimeInSideTransaction(opCtx());
    }

    // Mimic aborting the transaction.
    shard_role_details::setWriteUnitOfWork(opCtx(), nullptr);
    shard_role_details::getLocker(opCtx())->unsetMaxLockTimeout();
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onTransactionAbort(opCtx(), abortSlot);
    }
    txnParticipant.transitionToAbortedWithPrepareforTest(opCtx());

    repl::OplogInterfaceLocal oplogInterface(opCtx());
    auto oplogIter = oplogInterface.makeIterator();
    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected = BSON("abortTransaction" << 1);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT_FALSE(oplogEntry.shouldPrepare());
    }

    {
        auto oplogEntryObj = unittest::assertGet(oplogIter->next()).first;
        checkCommonFields(oplogEntryObj);
        OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
        auto o = oplogEntry.getObject();
        auto oExpected =
            BSON("applyOps" << BSON_ARRAY(BSON("op"
                                               << "i"
                                               << "ns" << nss.toString_forTest() << "ui" << uuid
                                               << "o" << doc << "o2" << docKey))
                            << "prepare" << true);
        ASSERT_BSONOBJ_EQ(oExpected, o);
        ASSERT(oplogEntry.shouldPrepare());
    }

    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest, TransactionalUnpreparedAbortTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0,
                        BSON("_id" << 0 << "data"
                                   << "x"));

    {
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        opObserver().onInserts(opCtx(),
                               *autoColl,
                               insert.begin(),
                               insert.end(),
                               /*recordIds=*/{},
                               /*fromMigrate=*/std::vector<bool>(insert.size(), false),
                               /*defaultFromMigrate=*/false);

        txnParticipant.transitionToAbortedWithoutPrepareforTest(opCtx());
        opObserver().onTransactionAbort(opCtx(), boost::none);
    }

    // Assert no oplog entries were written.
    repl::OplogInterfaceLocal oplogInterface(opCtx());
    auto oplogIter = oplogInterface.makeIterator();
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, oplogIter->next().getStatus());
}

TEST_F(OpObserverTransactionTest,
       PreparingEmptyTransactionLogsEmptyApplyOpsAndWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        prepareOpTime = reserveOpTimeInSideTransaction(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
        prepareTransaction({prepareOpTime}, prepareOpTime);
    }
    ASSERT_EQ(prepareOpTime.getTimestamp(),
              shard_role_details::getRecoveryUnit(opCtx())->getPrepareTimestamp());

    txnParticipant.stashTransactionResources(opCtx());
    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected = BSON("applyOps" << BSONArray() << "prepare" << true);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(oplogEntry.shouldPrepare());
    const auto startOpTime = oplogEntry.getOpTime();
    ASSERT_EQ(startOpTime.getTimestamp(), prepareOpTime.getTimestamp());
    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());

    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    assertTxnRecordStartOpTime(startOpTime);
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");
}

TEST_F(OpObserverTransactionTest, PreparingTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = reserveOpTimeInSideTransaction(opCtx());
        txnParticipant.transitionToPreparedforTest(opCtx(), slot);
        prepareOpTime = slot;
        prepareTransaction({slot}, prepareOpTime);
    }

    ASSERT_EQ(prepareOpTime.getTimestamp(),
              shard_role_details::getRecoveryUnit(opCtx())->getPrepareTimestamp());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverTransactionTest, AbortingUnpreparedTransactionDoesNotWriteToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    opObserver().onTransactionAbort(opCtx(), boost::none);
    txnParticipant.stashTransactionResources(opCtx());

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant.shutdown(opCtx());

    assertNoTxnRecord();
}

TEST_F(OpObserverTransactionTest, AbortingPreparedTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    OplogSlot abortSlot;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = reserveOpTimeInSideTransaction(opCtx());
        prepareTransaction({slot}, slot);
        txnParticipant.transitionToPreparedforTest(opCtx(), slot);
        abortSlot = reserveOpTimeInSideTransaction(opCtx());
    }

    // Mimic aborting the transaction.
    shard_role_details::setWriteUnitOfWork(opCtx(), nullptr);
    shard_role_details::getLocker(opCtx())->unsetMaxLockTimeout();
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onTransactionAbort(opCtx(), abortSlot);
        txnParticipant.transitionToAbortedWithPrepareforTest(opCtx());
    }
    txnParticipant.stashTransactionResources(opCtx());

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant.shutdown(opCtx());

    assertTxnRecord(txnNum(), {}, DurableTxnStateEnum::kAborted);
}

TEST_F(OpObserverTransactionTest, CommittingUnpreparedNonEmptyTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0,
                        BSON("_id" << 0 << "data"
                                   << "x"));

    {
        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        opObserver().onInserts(opCtx(),
                               *autoColl,
                               insert.begin(),
                               insert.end(),
                               /*recordIds=*/{},
                               /*fromMigrate=*/std::vector<bool>(insert.size(), false),
                               /*defaultFromMigrate=*/false);
    }

    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    shard_role_details::getWriteUnitOfWork(opCtx())->commit();

    assertTxnRecord(txnNum(), {}, DurableTxnStateEnum::kCommitted);
}

TEST_F(OpObserverTransactionTest,
       CommittingUnpreparedEmptyTransactionDoesNotWriteToTransactionTableOrOplog) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());

    txnParticipant.stashTransactionResources(opCtx());

    getNOplogEntries(opCtx(), 0);

    // Abort the storage-transaction without calling the OpObserver.
    txnParticipant.shutdown(opCtx());

    assertNoTxnRecord();
}

TEST_F(OpObserverTransactionTest, CommittingPreparedTransactionWritesToTransactionTable) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "prepareTransaction");

    repl::OpTime prepareOpTime;
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        WriteUnitOfWork wuow(opCtx());
        OplogSlot slot = reserveOpTimeInSideTransaction(opCtx());
        prepareOpTime = slot;
        prepareTransaction({slot}, slot);
        txnParticipant.transitionToPreparedforTest(opCtx(), slot);
    }

    OplogSlot commitSlot = reserveOpTimeInSideTransaction(opCtx());
    repl::OpTime commitOpTime = commitSlot;
    ASSERT_LTE(prepareOpTime, commitOpTime);

    // Mimic committing the transaction.
    shard_role_details::setWriteUnitOfWork(opCtx(), nullptr);
    shard_role_details::getLocker(opCtx())->unsetMaxLockTimeout();

    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onPreparedTransactionCommit(
            opCtx(),
            commitSlot,
            prepareOpTime.getTimestamp(),
            txnParticipant.retrieveCompletedTransactionOperations(opCtx())
                ->getOperationsForOpObserver());
    }
    assertTxnRecord(txnNum(), commitOpTime, DurableTxnStateEnum::kCommitted);
}

TEST_F(OpObserverTransactionTest, TransactionalInsertTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0,
                          BSON("_id" << 2 << "data"
                                     << "z"));
    inserts2.emplace_back(1,
                          BSON("_id" << 3 << "data"
                                     << "w"));
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    opObserver().onInserts(opCtx(),
                           *autoColl1,
                           inserts1.begin(),
                           inserts1.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
                           /*defaultFromMigrate=*/false);
    opObserver().onInserts(opCtx(),
                           *autoColl2,
                           inserts2.begin(),
                           inserts2.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts2.size(), false),
                           /*defaultFromMigrate=*/false);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();
    auto oExpected = BSON(
        "applyOps" << BSON_ARRAY(BSON("op"
                                      << "i"
                                      << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                      << BSON("_id" << 0 << "data"
                                                    << "x")
                                      << "o2" << BSON("_id" << 0))
                                 << BSON("op"
                                         << "i"
                                         << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                         << BSON("_id" << 1 << "data"
                                                       << "y")
                                         << "o2" << BSON("_id" << 1))
                                 << BSON("op"
                                         << "i"
                                         << "ns" << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                         << BSON("_id" << 2 << "data"
                                                       << "z")
                                         << "o2" << BSON("_id" << 2))
                                 << BSON("op"
                                         << "i"
                                         << "ns" << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                         << BSON("_id" << 3 << "data"
                                                       << "w")
                                         << "o2" << BSON("_id" << 3))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT(!oplogEntry.shouldPrepare());
    ASSERT_FALSE(oplogEntryObj.hasField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalInsertTestIncludesTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0,
                          BSON("_id" << 2 << "data"
                                     << "z"));
    inserts2.emplace_back(1,
                          BSON("_id" << 3 << "data"
                                     << "w"));

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    opObserver().onInserts(opCtx(),
                           *autoColl1,
                           inserts1.begin(),
                           inserts1.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
                           /*defaultFromMigrate=*/false);
    opObserver().onInserts(opCtx(),
                           *autoColl2,
                           inserts2.begin(),
                           inserts2.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts2.size(), false),
                           /*defaultFromMigrate=*/false);

    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());

    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();

    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "tid" << nss1.tenantId().value() << "ns"
                                           << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                           << BSON("_id" << 0 << "data"
                                                         << "x")
                                           << "o2" << BSON("_id" << 0))
                                      << BSON("op"
                                              << "i"
                                              << "tid" << nss1.tenantId().value() << "ns"
                                              << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                              << BSON("_id" << 1 << "data"
                                                            << "y")
                                              << "o2" << BSON("_id" << 1))
                                      << BSON("op"
                                              << "i"
                                              << "tid" << nss2.tenantId().value() << "ns"
                                              << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                              << BSON("_id" << 2 << "data"
                                                            << "z")
                                              << "o2" << BSON("_id" << 2))
                                      << BSON("op"
                                              << "i"
                                              << "tid" << nss2.tenantId().value() << "ns"
                                              << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                              << BSON("_id" << 3 << "data"
                                                            << "w")
                                              << "o2" << BSON("_id" << 3))));
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // This test assumes that the top level tenantId matches the tenantId in the first entry
    ASSERT_EQ(oplogEntry.getTid(), nss1.tenantId());
    ASSERT(!oplogEntry.shouldPrepare());
    ASSERT_FALSE(oplogEntryObj.hasField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalUpdateTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "update");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    const auto criteria1 = BSON("_id" << 0);
    const auto preImageDoc1 = criteria1;
    CollectionUpdateArgs updateArgs1{preImageDoc1};
    updateArgs1.criteria = criteria1;
    updateArgs1.stmtIds = {0};
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    OplogUpdateEntryArgs update1(&updateArgs1, *autoColl1);

    const auto criteria2 = BSON("_id" << 1);
    const auto preImageDoc2 = criteria2;
    CollectionUpdateArgs updateArgs2{preImageDoc2};
    updateArgs2.criteria = criteria2;
    updateArgs2.stmtIds = {1};
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    OplogUpdateEntryArgs update2(&updateArgs2, *autoColl2);

    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    auto oplogEntry = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntry);
    auto o = oplogEntry.getObjectField("o");
    auto oExpected = BSON(
        "applyOps" << BSON_ARRAY(BSON("op"
                                      << "u"
                                      << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                      << BSON("$set" << BSON("data"
                                                             << "x"))
                                      << "o2" << BSON("_id" << 0))
                                 << BSON("op"
                                         << "u"
                                         << "ns" << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                         << BSON("$set" << BSON("data"
                                                                << "y"))
                                         << "o2" << BSON("_id" << 1))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_FALSE(oplogEntry.hasField("prepare"));
    ASSERT_FALSE(oplogEntry.getBoolField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalUpdateTestIncludesTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "update");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    const auto criteria1 = BSON("_id" << 0);
    const auto preImageDoc1 = criteria1;
    CollectionUpdateArgs updateArgs1{preImageDoc1};
    updateArgs1.criteria = criteria1;
    updateArgs1.stmtIds = {0};
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    OplogUpdateEntryArgs update1(&updateArgs1, *autoColl1);

    const auto criteria2 = BSON("_id" << 1);
    const auto preImageDoc2 = criteria2;
    CollectionUpdateArgs updateArgs2{preImageDoc2};
    updateArgs2.criteria = criteria2;
    updateArgs2.stmtIds = {1};
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    OplogUpdateEntryArgs update2(&updateArgs2, *autoColl2);

    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);

    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());

    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();

    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "u"
                                           << "tid" << nss1.tenantId().value() << "ns"
                                           << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                           << BSON("$set" << BSON("data"
                                                                  << "x"))
                                           << "o2" << BSON("_id" << 0))
                                      << BSON("op"
                                              << "u"
                                              << "tid" << nss2.tenantId().value() << "ns"
                                              << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                              << BSON("$set" << BSON("data"
                                                                     << "y"))
                                              << "o2" << BSON("_id" << 1))));
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // This test assumes that the top level tenantId matches the tenantId in the first entry
    ASSERT_EQ(oplogEntry.getTid(), nss1.tenantId());
    ASSERT_FALSE(oplogEntryObj.hasField("prepare"));
    ASSERT_FALSE(oplogEntryObj.getBoolField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalDeleteTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "delete");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    OplogDeleteEntryArgs args;

    auto doc1 = BSON("_id" << 0 << "data"
                           << "x");
    opObserver().aboutToDelete(opCtx(), *autoColl1, doc1, &args);
    opObserver().onDelete(opCtx(), *autoColl1, 0, doc1, args);

    auto doc2 = BSON("_id" << 1 << "data"
                           << "y");
    opObserver().aboutToDelete(opCtx(), *autoColl2, doc2, &args);
    opObserver().onDelete(opCtx(), *autoColl2, 0, doc2, args);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    auto oplogEntry = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntry);
    auto o = oplogEntry.getObjectField("o");
    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "d"
                                           << "ns" << nss1.toString_forTest() << "ui" << uuid1
                                           << "o" << BSON("_id" << 0))
                                      << BSON("op"
                                              << "d"
                                              << "ns" << nss2.toString_forTest() << "ui" << uuid2
                                              << "o" << BSON("_id" << 1))));
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_FALSE(oplogEntry.hasField("prepare"));
    ASSERT_FALSE(oplogEntry.getBoolField("prepare"));
}

TEST_F(OpObserverTransactionTest, TransactionalDeleteTestIncludesTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "delete");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    OplogDeleteEntryArgs args;

    auto doc1 = BSON("_id" << 0 << "data"
                           << "x");
    opObserver().aboutToDelete(opCtx(), *autoColl1, doc1, &args);
    opObserver().onDelete(opCtx(), *autoColl1, 0, doc1, args);

    auto doc2 = BSON("_id" << 1 << "data"
                           << "y");
    opObserver().aboutToDelete(opCtx(), *autoColl2, doc2, &args);
    opObserver().onDelete(opCtx(), *autoColl2, 0, doc2, args);

    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());

    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    checkCommonFields(oplogEntryObj);
    OplogEntry oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    auto o = oplogEntry.getObject();

    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "d"
                                           << "tid" << nss1.tenantId().value() << "ns"
                                           << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                           << BSON("_id" << 0))
                                      << BSON("op"
                                              << "d"
                                              << "tid" << nss2.tenantId().value() << "ns"
                                              << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                              << BSON("_id" << 1))));
    ASSERT_BSONOBJ_EQ(oExpected, o);

    // This test assumes that the top level tenantId matches the tenantId in the first entry
    ASSERT_EQ(oplogEntry.getTid(), nss1.tenantId());
    ASSERT_FALSE(oplogEntryObj.hasField("prepare"));
    ASSERT_FALSE(oplogEntryObj.getBoolField("prepare"));
}

class OpObserverServerlessTransactionTest : public OpObserverTransactionTest {
private:
    // Needs to override to set serverless mode.
    repl::ReplSettings createReplSettings() override {
        return repl::createServerlessReplSettings();
    }
};

TEST_F(OpObserverServerlessTransactionTest,
       OnUnpreparedTransactionCommitChecksIfTenantMigrationIsBlockingWrites) {
    // Add a tenant migration access blocker on donor for blocking writes.
    auto donorMtab = std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    std::vector<InsertStatement> insert;
    insert.emplace_back(0,
                        BSON("_id" << 0 << "data"
                                   << "x"));

    {
        AutoGetCollection autoColl(opCtx(), kNssUnderTenantId, MODE_IX);
        opObserver().onInserts(opCtx(),
                               *autoColl,
                               insert.begin(),
                               insert.end(),
                               /*recordIds=*/{},
                               /*fromMigrate=*/std::vector<bool>(insert.size(), false),
                               /*defaultFromMigrate=*/false);
    }

    donorMtab->startBlockingWrites();

    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    ASSERT_THROWS_CODE(commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver()),
                       DBException,
                       ErrorCodes::TenantMigrationConflict);

    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).shutDown();
}

/**
 * Test fixture for testing OpObserver behavior specific to retryable findAndModify.
 */
class OpObserverRetryableFindAndModifyTest : public OpObserverTxnParticipantTest {
public:
    void tearDown() override {
        OpObserverTxnParticipantTest::tearDown();
    }

    void setUp() override {
        OpObserverTest::setUp();
        auto opCtx = cc().makeOperationContext();
        reset(opCtx.get(), nss, uuid);
    }

protected:
    void testRetryableFindAndModifyUpdateRequestingPostImageHasNeedsRetryImage() {
        NamespaceString nss = NamespaceString::createNamespaceString_forTest(
            boost::none /* tenantId */, "test", "coll");
        const auto criteria = BSON("_id" << 0);
        const auto preImageDoc = criteria;
        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.criteria = criteria;
        updateArgs.stmtIds = {0};
        updateArgs.updatedDoc = BSON("_id" << 0 << "data"
                                           << "x");
        updateArgs.update = BSON("$set" << BSON("data"
                                                << "x"));
        updateArgs.storeDocOption = CollectionUpdateArgs::StoreDocOption::PostImage;

        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        OplogUpdateEntryArgs update(&updateArgs, *autoColl);
        update.retryableFindAndModifyLocation = RetryableFindAndModifyLocation::kSideCollection;

        opObserver().onUpdate(opCtx(), update);
        commit();

        // Asserts that only a single oplog entry was created. In essence, we did not create any
        // no-op image entries in the oplog.
        const auto oplogEntry = assertGetSingleOplogEntry();
        ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPreImageOpTimeFieldName));
        ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPostImageOpTimeFieldName));
        ASSERT_TRUE(oplogEntry.hasField(repl::OplogEntryBase::kNeedsRetryImageFieldName));
        ASSERT_EQUALS(oplogEntry.getStringField(repl::OplogEntryBase::kNeedsRetryImageFieldName),
                      "postImage"_sd);

        finish();
    }

    void testRetryableFindAndModifyUpdateRequestingPreImageHasNeedsRetryImage() {
        NamespaceString nss =
            NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
        const auto criteria = BSON("_id" << 0);
        const auto preImageDoc = BSON("_id" << 0 << "data"
                                            << "y");
        CollectionUpdateArgs updateArgs{preImageDoc};
        updateArgs.criteria = criteria;
        updateArgs.stmtIds = {0};
        updateArgs.update = BSON("$set" << BSON("data"
                                                << "x"));
        updateArgs.storeDocOption = CollectionUpdateArgs::StoreDocOption::PreImage;

        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        OplogUpdateEntryArgs update(&updateArgs, *autoColl);
        update.retryableFindAndModifyLocation = RetryableFindAndModifyLocation::kSideCollection;

        opObserver().onUpdate(opCtx(), update);
        commit();

        // Asserts that only a single oplog entry was created. In essence, we did not create any
        // no-op image entries in the oplog.
        const auto oplogEntry = assertGetSingleOplogEntry();
        ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPreImageOpTimeFieldName));
        ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPostImageOpTimeFieldName));
        ASSERT_TRUE(oplogEntry.hasField(repl::OplogEntryBase::kNeedsRetryImageFieldName));
        ASSERT_EQUALS(oplogEntry.getStringField(repl::OplogEntryBase::kNeedsRetryImageFieldName),
                      "preImage"_sd);

        finish();
    }

    void testRetryableFindAndModifyDeleteHasNeedsRetryImage() {


        AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
        const auto deletedDoc = BSON("_id" << 0 << "data"
                                           << "x");
        OplogDeleteEntryArgs args;
        args.retryableFindAndModifyLocation = RetryableFindAndModifyLocation::kSideCollection;

        opObserver().aboutToDelete(opCtx(), *autoColl, deletedDoc, &args);
        opObserver().onDelete(opCtx(), *autoColl, 0, deletedDoc, args);
        commit();

        // Asserts that only a single oplog entry was created. In essence, we did not create any
        // no-op image entries in the oplog.
        const auto oplogEntry = assertGetSingleOplogEntry();
        ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPreImageOpTimeFieldName));
        ASSERT_FALSE(oplogEntry.hasField(repl::OplogEntryBase::kPostImageOpTimeFieldName));
        ASSERT_TRUE(oplogEntry.hasField(repl::OplogEntryBase::kNeedsRetryImageFieldName));
        ASSERT_EQUALS(oplogEntry.getStringField(repl::OplogEntryBase::kNeedsRetryImageFieldName),
                      "preImage"_sd);

        finish();
    }

    NamespaceString nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    UUID uuid = UUID::gen();

    virtual void commit() = 0;

    virtual void finish() {}

    virtual BSONObj assertGetSingleOplogEntry() = 0;
};

class OpObserverRetryableFindAndModifyOutsideTransactionTest
    : public OpObserverRetryableFindAndModifyTest {
public:
    void setUp() override {
        OpObserverRetryableFindAndModifyTest::setUp();
        setUpObserverContext();
        setUpRetryableWrite();
    }

protected:
    void commit() final{};

    BSONObj assertGetSingleOplogEntry() final {
        return getSingleOplogEntry(opCtx());
    }
};

TEST_F(OpObserverRetryableFindAndModifyOutsideTransactionTest,
       RetryableFindAndModifyUpdateRequestingPostImageHasNeedsRetryImage) {
    WriteUnitOfWork wuow{opCtx()};
    testRetryableFindAndModifyUpdateRequestingPostImageHasNeedsRetryImage();
}

TEST_F(OpObserverRetryableFindAndModifyOutsideTransactionTest,
       RetryableFindAndModifyUpdateRequestingPreImageHasNeedsRetryImage) {
    WriteUnitOfWork wuow{opCtx()};
    testRetryableFindAndModifyUpdateRequestingPreImageHasNeedsRetryImage();
}

TEST_F(OpObserverRetryableFindAndModifyOutsideTransactionTest,
       RetryableFindAndModifyDeleteHasNeedsRetryImage) {
    WriteUnitOfWork wuow{opCtx()};
    testRetryableFindAndModifyDeleteHasNeedsRetryImage();
}

class OpObserverRetryableFindAndModifyInsideUnpreparedRetryableInternalTransactionTest
    : public OpObserverRetryableFindAndModifyTest {
public:
    void setUp() override {
        OpObserverRetryableFindAndModifyTest::setUp();
        setUpObserverContext();
        setUpRetryableInternalTransaction();
    }

protected:
    void commit() final {
        commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    };

    BSONObj assertGetSingleOplogEntry() final {
        return getInnerEntryFromSingleApplyOpsOplogEntry(opCtx());
    }
};

TEST_F(OpObserverRetryableFindAndModifyInsideUnpreparedRetryableInternalTransactionTest,
       RetryableFindAndModifyUpdateRequestingPostImageHasNeedsRetryImage) {
    WriteUnitOfWork wuow{opCtx()};
    testRetryableFindAndModifyUpdateRequestingPostImageHasNeedsRetryImage();
}

TEST_F(OpObserverRetryableFindAndModifyInsideUnpreparedRetryableInternalTransactionTest,
       RetryableFindAndModifyUpdateRequestingPreImageHasNeedsRetryImage) {
    WriteUnitOfWork wuow{opCtx()};
    testRetryableFindAndModifyUpdateRequestingPreImageHasNeedsRetryImage();
}

TEST_F(OpObserverRetryableFindAndModifyInsideUnpreparedRetryableInternalTransactionTest,
       RetryableFindAndModifyDeleteHasNeedsRetryImage) {
    WriteUnitOfWork wuow{opCtx()};
    testRetryableFindAndModifyDeleteHasNeedsRetryImage();
}

class OpObserverRetryableFindAndModifyInsidePreparedRetryableInternalTransactionTest
    : public OpObserverRetryableFindAndModifyTest {
public:
    void setUp() override {
        OpObserverRetryableFindAndModifyTest::setUp();
        setUpObserverContext();
        setUpRetryableInternalTransaction();
    }

protected:
    void commit() final {
        const auto reservedOplogSlots = reserveOpTimesInSideTransaction(
            opCtx(), 1 + txnParticipant().getNumberOfPrePostImagesToWriteForTest());
        invariant(reservedOplogSlots.size() >= 1);
        const auto prepareSlot = reservedOplogSlots.back();
        txnParticipant().transitionToPreparedforTest(opCtx(), prepareSlot);
        prepareTransaction(reservedOplogSlots,
                           prepareSlot,
                           txnParticipant().getNumberOfPrePostImagesToWriteForTest());
        TransactionParticipant::get(opCtx()).stashTransactionResources(opCtx());
    };

    void finish() final {
        TransactionParticipant::get(opCtx()).unstashTransactionResources(opCtx(),
                                                                         "abortTransaction");
    }

    BSONObj assertGetSingleOplogEntry() final {
        return getInnerEntryFromSingleApplyOpsOplogEntry(opCtx());
    }
};

TEST_F(OpObserverRetryableFindAndModifyInsidePreparedRetryableInternalTransactionTest,
       RetryableFindAndModifyUpdateRequestingPostImageHasNeedsRetryImage) {
    TransactionParticipant::get(opCtx()).unstashTransactionResources(opCtx(), "update");
    testRetryableFindAndModifyUpdateRequestingPostImageHasNeedsRetryImage();
}

TEST_F(OpObserverRetryableFindAndModifyInsidePreparedRetryableInternalTransactionTest,
       RetryableFindAndModifyUpdateRequestingPreImageHasNeedsRetryImage) {
    TransactionParticipant::get(opCtx()).unstashTransactionResources(opCtx(), "update");
    testRetryableFindAndModifyUpdateRequestingPreImageHasNeedsRetryImage();
}

TEST_F(OpObserverRetryableFindAndModifyInsidePreparedRetryableInternalTransactionTest,
       RetryableFindAndModifyDeleteHasNeedsRetryImage) {
    TransactionParticipant::get(opCtx()).unstashTransactionResources(opCtx(), "delete");
    testRetryableFindAndModifyDeleteHasNeedsRetryImage();
}

boost::optional<OplogEntry> findByTimestamp(const std::vector<BSONObj>& oplogs, Timestamp ts) {
    for (auto& oplog : oplogs) {
        const auto& entry = assertGet(OplogEntry::parse(oplog));
        if (entry.getTimestamp() == ts) {
            return entry;
        }
    }
    return boost::none;
}

using StoreDocOption = CollectionUpdateArgs::StoreDocOption;

namespace {
const auto kNonFaM = StoreDocOption::None;
const auto kFaMPre = StoreDocOption::PreImage;
const auto kFaMPost = StoreDocOption::PostImage;

const bool kChangeStreamImagesEnabled = true;
const bool kChangeStreamImagesDisabled = false;

const auto kNotRetryable = RetryableFindAndModifyLocation::kNone;
const auto kRecordInSideCollection = RetryableFindAndModifyLocation::kSideCollection;

const std::vector<bool> kInMultiDocumentTransactionCases{false, true};
}  // namespace

struct UpdateTestCase {
    StoreDocOption imageType;
    bool changeStreamImagesEnabled;
    RetryableFindAndModifyLocation retryableOptions;

    int numOutputOplogs;

    bool isFindAndModify() const {
        return imageType != StoreDocOption::None;
    }

    bool isRetryable() const {
        return retryableOptions != kNotRetryable;
    }

    std::string getImageTypeStr() const {
        switch (imageType) {
            case StoreDocOption::None:
                return "None";
            case StoreDocOption::PreImage:
                return "PreImage";
            case StoreDocOption::PostImage:
                return "PostImage";
        }
        MONGO_UNREACHABLE;
    }

    std::string getRetryableFindAndModifyLocationStr() const {
        switch (retryableOptions) {
            case kNotRetryable:
                return "Not retryable";
            case kRecordInSideCollection:
                return "Images in side collection";
        }
        MONGO_UNREACHABLE;
    }
};

class OnUpdateOutputsTest : public OpObserverTest {
protected:
    void logTestCase(const UpdateTestCase& testCase) {
        LOGV2(5739902,
              "UpdateTestCase",
              "ImageType"_attr = testCase.getImageTypeStr(),
              "ChangeStreamPreAndPostImagesEnabled"_attr = testCase.changeStreamImagesEnabled,
              "RetryableFindAndModifyLocation"_attr =
                  testCase.getRetryableFindAndModifyLocationStr(),
              "ExpectedOplogEntries"_attr = testCase.numOutputOplogs);
    }

    void initializeOplogUpdateEntryArgs(OperationContext* opCtx,
                                        const UpdateTestCase& testCase,
                                        OplogUpdateEntryArgs* update) {
        update->updateArgs->changeStreamPreAndPostImagesEnabledForCollection =
            testCase.changeStreamImagesEnabled;

        switch (testCase.retryableOptions) {
            case kNotRetryable:
                update->updateArgs->stmtIds = {kUninitializedStmtId};
                break;
            case kRecordInSideCollection:
                update->retryableFindAndModifyLocation =
                    RetryableFindAndModifyLocation::kSideCollection;
                update->updateArgs->stmtIds = {1};
                if (testCase.retryableOptions == kRecordInSideCollection) {
                    // 'getNextOpTimes' requires us to be inside a WUOW when reserving oplog slots.
                    WriteUnitOfWork wuow(opCtx);
                    auto reservedSlots = repl::getNextOpTimes(opCtx, 3);
                    update->updateArgs->oplogSlots = reservedSlots;
                    wuow.commit();
                }
                break;
        }
        update->updateArgs->updatedDoc = kPostImageDoc;
        update->updateArgs->update = kUpdate;
        update->updateArgs->storeDocOption = testCase.imageType;
    }

    void checkSideCollectionIfNeeded(
        OperationContext* opCtx,
        const UpdateTestCase& testCase,
        const OplogUpdateEntryArgs& update,
        const std::vector<BSONObj>& oplogs,
        const OplogEntry& updateOplogEntry,
        const boost::optional<OplogEntry>& applyOpsOplogEntry = boost::none) {
        bool checkSideCollection =
            testCase.isFindAndModify() && testCase.retryableOptions == kRecordInSideCollection;
        if (checkSideCollection) {
            repl::ImageEntry imageEntry =
                getImageEntryFromSideCollection(opCtx, *updateOplogEntry.getSessionId());
            const BSONObj& expectedImage = testCase.imageType == StoreDocOption::PreImage
                ? update.updateArgs->preImageDoc
                : update.updateArgs->updatedDoc;
            ASSERT_BSONOBJ_EQ(expectedImage, imageEntry.getImage());
            ASSERT(imageEntry.getImageKind() == updateOplogEntry.getNeedsRetryImage());
            if (applyOpsOplogEntry) {
                ASSERT_FALSE(applyOpsOplogEntry->getNeedsRetryImage());
            }
            if (testCase.imageType == StoreDocOption::PreImage) {
                ASSERT(imageEntry.getImageKind() == repl::RetryImageEnum::kPreImage);
            } else {
                ASSERT(imageEntry.getImageKind() == repl::RetryImageEnum::kPostImage);
            }

            // If 'updateOplogEntry' has opTime T, opTime T-1 must be reserved for potential
            // forged noop oplog entry for the pre/postImage written to the side collection.
            const Timestamp forgeNoopTimestamp = updateOplogEntry.getTimestamp() - 1;
            ASSERT_FALSE(findByTimestamp(oplogs, forgeNoopTimestamp));
        } else {
            ASSERT_FALSE(updateOplogEntry.getNeedsRetryImage());
            if (updateOplogEntry.getSessionId()) {
                ASSERT_FALSE(
                    didWriteImageEntryToSideCollection(opCtx, *updateOplogEntry.getSessionId()));
            } else {
                // Session id is missing only for non-retryable option.
                ASSERT(testCase.retryableOptions == kNotRetryable);
            }
        }
    }

    void checkChangeStreamImagesIfNeeded(OperationContext* opCtx,
                                         const UpdateTestCase& testCase,
                                         const OplogUpdateEntryArgs& update,
                                         const OplogEntry& updateOplogEntry) {
        if (testCase.changeStreamImagesEnabled) {
            BSONObj container;
            ChangeStreamPreImageId preImageId(
                _uuid, updateOplogEntry.getOpTime().getTimestamp(), 0);
            ChangeStreamPreImage preImage = getChangeStreamPreImage(opCtx, preImageId, &container);
            const BSONObj& expectedImage = update.updateArgs->preImageDoc;
            ASSERT_BSONOBJ_EQ(expectedImage, preImage.getPreImage());
            ASSERT_EQ(updateOplogEntry.getWallClockTime(), preImage.getOperationTime());
        }
    }

    void setUp() override {
        OpObserverTest::setUp();
        auto opCtx = cc().makeOperationContext();
        reset(opCtx.get(), _nss, _uuid);
    }

    std::vector<UpdateTestCase> _cases = {
        // Regular updates.
        {kNonFaM, kChangeStreamImagesDisabled, kNotRetryable, 1},
        {kNonFaM, kChangeStreamImagesEnabled, kNotRetryable, 1},
        {kNonFaM, kChangeStreamImagesEnabled, kRecordInSideCollection, 1},
        // FindAndModify asking for a preImage.
        {kFaMPre, kChangeStreamImagesDisabled, kNotRetryable, 1},
        {kFaMPre, kChangeStreamImagesDisabled, kRecordInSideCollection, 1},
        {kFaMPre, kChangeStreamImagesEnabled, kNotRetryable, 1},
        {kFaMPre, kChangeStreamImagesEnabled, kRecordInSideCollection, 1},
        // FindAndModify asking for a postImage.
        {kFaMPost, kChangeStreamImagesDisabled, kNotRetryable, 1},
        {kFaMPost, kChangeStreamImagesDisabled, kRecordInSideCollection, 1},
        {kFaMPost, kChangeStreamImagesEnabled, kNotRetryable, 1},
        {kFaMPost, kChangeStreamImagesEnabled, kRecordInSideCollection, 1}};

    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    const UUID _uuid = UUID::gen();

    const BSONObj kCriteria = BSON("_id" << 0);
    const BSONObj kPreImageDoc = BSON("_id" << 0 << "preImage" << true);
    const BSONObj kPostImageDoc = BSON("_id" << 0 << "postImage" << true);
    const BSONObj kUpdate =
        BSON("$set" << BSON("postImage" << true) << "$unset" << BSON("preImage" << 1));
};

TEST_F(OnUpdateOutputsTest, TestNonTransactionFundamentalOnUpdateOutputs) {
    // Create a registry that registers the OpObserverImpl, FindAndModifyImagesOpObserver, and
    // ChangeStreamPreImagesOpObserver.
    // These OpObservers work together to ensure that images for retryable findAndModify and
    // change streams are written correctly to the respective side collections.
    // It falls into cases where `ReservedTimes` is expected to be instantiated. Due to strong
    // encapsulation, we use the registry that managers the `ReservedTimes` on our behalf.
    OpObserverRegistry opObserver;
    opObserver.addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    opObserver.addObserver(std::make_unique<FindAndModifyImagesOpObserver>());
    opObserver.addObserver(std::make_unique<ChangeStreamPreImagesOpObserver>());

    for (std::size_t testIdx = 0; testIdx < _cases.size(); ++testIdx) {
        const auto& testCase = _cases[testIdx];
        logTestCase(testCase);

        auto opCtxRaii = cc().makeOperationContext();
        OperationContext* opCtx = opCtxRaii.get();

        // Phase 1: Clearing any state and setting up fixtures/the update call.
        resetOplogAndTransactions(opCtx);

        std::unique_ptr<MongoDSessionCatalog::Session> contextSession;
        if (testCase.isRetryable()) {
            beginRetryableWriteWithTxnNumber(opCtx, testIdx, contextSession);
        }

        // Phase 2: Call the code we're testing.
        CollectionUpdateArgs updateArgs{kPreImageDoc};
        updateArgs.criteria = kCriteria;

        WriteUnitOfWork wuow(opCtx);
        AutoGetCollection locks(opCtx, _nss, MODE_IX);
        OplogUpdateEntryArgs updateEntryArgs(&updateArgs, *locks);
        initializeOplogUpdateEntryArgs(opCtx, testCase, &updateEntryArgs);
        opObserver.onUpdate(opCtx, updateEntryArgs);
        wuow.commit();

        // Phase 3: Analyze the results:
        // This `getNOplogEntries` also asserts that all oplogs are retrieved.
        std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, testCase.numOutputOplogs);
        // Entries are returned in ascending timestamp order.
        auto updateOplogEntry = assertGet(OplogEntry::parse(oplogs.back()));
        checkSideCollectionIfNeeded(opCtx, testCase, updateEntryArgs, oplogs, updateOplogEntry);
        checkChangeStreamImagesIfNeeded(opCtx, testCase, updateEntryArgs, updateOplogEntry);
    }
}

TEST_F(OnUpdateOutputsTest, TestFundamentalTransactionOnUpdateOutputs) {
    // Create a registry that registers the OpObserverImpl, FindAndModifyImagesOpObserver, and
    // ChangeStreamPreImagesOpObserver.
    // These OpObservers work together to ensure that images for retryable findAndModify and
    // change streams are written correctly to the respective side collections.
    // It falls into cases where `ReservedTimes` is expected to be instantiated. Due to strong
    // encapsulation, we use the registry that managers the `ReservedTimes` on our behalf.
    OpObserverRegistry opObserver;
    opObserver.addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    opObserver.addObserver(std::make_unique<FindAndModifyImagesOpObserver>());
    opObserver.addObserver(std::make_unique<ChangeStreamPreImagesOpObserver>());

    for (std::size_t testIdx = 0; testIdx < _cases.size(); ++testIdx) {
        const auto& testCase = _cases[testIdx];
        logTestCase(testCase);

        auto opCtxRaii = cc().makeOperationContext();
        OperationContext* opCtx = opCtxRaii.get();

        // Phase 1: Clearing any state and setting up fixtures/the update call.
        resetOplogAndTransactions(opCtx);

        std::unique_ptr<MongoDSessionCatalog::Session> contextSession;
        if (testCase.isRetryable()) {
            beginRetryableInternalTransactionWithTxnNumber(opCtx, testIdx, contextSession);
        } else {
            beginNonRetryableTransactionWithTxnNumber(opCtx, testIdx, contextSession);
        }

        // Phase 2: Call the code we're testing.
        WriteUnitOfWork wuow(opCtx);
        AutoGetCollection locks(opCtx, _nss, MODE_IX);
        CollectionUpdateArgs updateArgs{kPreImageDoc};
        updateArgs.criteria = kCriteria;
        OplogUpdateEntryArgs updateEntryArgs(&updateArgs, *locks);
        initializeOplogUpdateEntryArgs(opCtx, testCase, &updateEntryArgs);

        opObserver.onUpdate(opCtx, updateEntryArgs);
        commitUnpreparedTransaction<OpObserverRegistry>(opCtx, opObserver);
        wuow.commit();

        // Phase 3: Analyze the results:
        // This `getNOplogEntries` also asserts that all oplogs are retrieved.
        std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, testCase.numOutputOplogs);
        // Entries are returned in ascending timestamp order.
        auto applyOpsOplogEntry = assertGet(OplogEntry::parse(oplogs.back()));
        auto updateOplogEntry = getInnerEntryFromApplyOpsOplogEntry(applyOpsOplogEntry);
        checkSideCollectionIfNeeded(
            opCtx, testCase, updateEntryArgs, oplogs, updateOplogEntry, applyOpsOplogEntry);
        checkChangeStreamImagesIfNeeded(opCtx, testCase, updateEntryArgs, updateOplogEntry);
    }
}

struct InsertTestCase {
    bool isRetryableWrite;
    int numDocsToInsert;
};
TEST_F(OpObserverTest, TestFundamentalOnInsertsOutputs) {
    // Create a registry that only registers the Impl. It can be challenging to call methods on
    // the Impl directly. It falls into cases where `ReservedTimes` is expected to be instantiated.
    // Due to strong encapsulation, we use the registry that managers the `ReservedTimes` on our
    // behalf.
    OpObserverRegistry opObserver;
    opObserver.addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));

    const bool isRetryableWrite = true;
    const bool isNotRetryableWrite = false;
    const int oneDoc = 1;
    const int threeDocs = 3;

    std::vector<InsertTestCase> cases{{isNotRetryableWrite, oneDoc},
                                      {isNotRetryableWrite, threeDocs},
                                      {isRetryableWrite, oneDoc},
                                      {isRetryableWrite, threeDocs}};

    for (std::size_t testIdx = 0; testIdx < cases.size(); ++testIdx) {
        const auto& testCase = cases[testIdx];
        LOGV2(5739904,
              "InsertTestCase",
              "Retryable"_attr = testCase.isRetryableWrite,
              "NumDocsToInsert"_attr = testCase.numDocsToInsert);

        auto opCtxRaii = cc().makeOperationContext();
        OperationContext* opCtx = opCtxRaii.get();
        // Phase 1: Clearing any state and setting up fixtures/the update call.
        resetOplogAndTransactions(opCtx);

        std::vector<InsertStatement> toInsert;
        for (int stmtIdx = 0; stmtIdx < testCase.numDocsToInsert; ++stmtIdx) {
            StmtId stmtId = testCase.isRetryableWrite ? StmtId(stmtIdx) : kUninitializedStmtId;
            toInsert.emplace_back(stmtId, BSON("_id" << stmtIdx));
        }

        std::unique_ptr<MongoDSessionCatalog::Session> contextSession;
        if (testCase.isRetryableWrite) {
            beginRetryableWriteWithTxnNumber(opCtx, testIdx, contextSession);
        }

        // Phase 2: Call the code we're testing.
        AutoGetCollection autoColl(opCtx, nss, MODE_IX);
        WriteUnitOfWork wuow(opCtx);
        opObserver.onInserts(opCtx,
                             *autoColl,
                             toInsert.begin(),
                             toInsert.end(),
                             /*recordIds=*/{},
                             /*fromMigrate=*/std::vector<bool>(toInsert.size(), false),
                             /*defaultFromMigrate=*/false);
        wuow.commit();

        // Phase 3: Analyze the results:
        // ----
        // This `getNOplogEntries` also asserts that all oplogs are retrieved.
        std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, toInsert.size());
        // Entries are returned in ascending timestamp order.
        for (std::size_t opIdx = 0; opIdx < oplogs.size(); ++opIdx) {
            const repl::OplogEntry& entry = assertGet(repl::OplogEntry::parse(oplogs[opIdx]));

            ASSERT_BSONOBJ_EQ(entry.getObject(), BSON("_id" << static_cast<int>(opIdx)));
            if (!testCase.isRetryableWrite) {
                ASSERT_FALSE(entry.getSessionId());
                ASSERT_FALSE(entry.getTxnNumber());
                ASSERT_EQ(0, entry.getStatementIds().size());
                continue;
            }

            // Only for retryable writes:
            ASSERT_EQ(opCtx->getLogicalSessionId().value(), entry.getSessionId().value());
            ASSERT_EQ(opCtx->getTxnNumber().value(), entry.getTxnNumber().value());
            ASSERT_EQ(1, entry.getStatementIds().size());
            ASSERT_EQ(StmtId(opIdx), entry.getStatementIds()[0]);
            // When we insert multiple documents in retryable writes, each insert will "link"
            // back to the previous insert. This code verifies that C["prevOpTime"] -> B and
            // B["prevOpTime"] -> A.
            Timestamp expectedPrevWriteOpTime = Timestamp(0, 0);
            if (opIdx > 0) {
                expectedPrevWriteOpTime =
                    oplogs[opIdx - 1][repl::OplogEntryBase::kTimestampFieldName].timestamp();
            }
            ASSERT_EQ(expectedPrevWriteOpTime,
                      entry.getPrevWriteOpTimeInTransaction().value().getTimestamp());
        }

        if (testCase.isRetryableWrite) {
            // Also assert for retryable writes that the `config.transactions` entry's
            // `lastWriteOpTime` and `txnNum` reflects the latest oplog entry.
            SessionTxnRecord transactionRecord = getTxnRecord(opCtx, *opCtx->getLogicalSessionId());
            ASSERT_EQ(oplogs.back()[repl::OplogEntryBase::kTimestampFieldName].timestamp(),
                      transactionRecord.getLastWriteOpTime().getTimestamp());
            ASSERT_EQ(oplogs.back()[repl::OplogEntryBase::kTxnNumberFieldName].Long(),
                      transactionRecord.getTxnNum());
        }
    }
}

struct DeleteTestCase {
    bool changeStreamImagesEnabled;
    RetryableFindAndModifyLocation retryableOptions;

    int numOutputOplogs;

    bool isRetryable() const {
        return retryableOptions != kNotRetryable;
    }

    std::string getRetryableFindAndModifyLocationStr() const {
        switch (retryableOptions) {
            case kNotRetryable:
                return "Not retryable";
            case kRecordInSideCollection:
                return "Images in side collection";
        }
        MONGO_UNREACHABLE;
    }
};

class BatchedWriteOutputsTest : public OpObserverTest {
public:
    void setUp() override {
        OpObserverTest::setUp();

        auto opObserverRegistry = std::make_unique<OpObserverRegistry>();
        opObserverRegistry->addObserver(
            std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
        getServiceContext()->setOpObserver(std::move(opObserverRegistry));
    }

protected:
    // The maximum numbers of documents that can be deleted in a batch. Assumes _id of integer type.
    // This limit should be chosen such that the total size of delete operations stays under the
    // BSON 16 MB limit.
    // In practice, the number of delete operations is controlled by the server parameters
    // batchedDeletesTargetStagedDocBytes and batchedDeletesTargetBatchDocs, which have much
    // lower defaults.
    // See maxNumberOfBatchedOperationsInSingleOplogEntry and
    // maxSizeOfBatchedOperationsInSingleOplogEntryBytes.
    static const int maxDeleteOpsInBatch = 202000;
    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    const NamespaceString _nssWithTid =
        NamespaceString::createNamespaceString_forTest(TenantId(OID::gen()), "test", "coll");
};

// Verifies that a WriteUnitOfWork with groupOplogEntries=kGroupForTransaction replicates its writes
// as a single applyOps. Tests WUOWs batching a range of 1 to 5 deletes (inclusive).
TEST_F(BatchedWriteOutputsTest, TestApplyOpsGrouping) {
    const auto nDocsToDelete = 5;
    const BSONObj docsToDelete[nDocsToDelete] = {
        BSON("_id" << 0),
        BSON("_id" << 1),
        BSON("_id" << 2),
        BSON("_id" << 3),
        BSON("_id" << 4),
    };

    // Setup.
    auto opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    reset(opCtx, _nss);
    reset(opCtx, NamespaceString::kRsOplogNamespace);

    // Run the test with WUOW's grouping 1 to 5 deletions.
    for (size_t docsToBeBatched = 1; docsToBeBatched <= nDocsToDelete; docsToBeBatched++) {

        // Start a WUOW with groupOplogEntries=kGroupForTransaction. Verify that initialises the
        // BatchedWriteContext.
        auto& bwc = BatchedWriteContext::get(opCtx);
        ASSERT(!bwc.writesAreBatched());
        WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
        ASSERT(bwc.writesAreBatched());

        AutoGetCollection autoColl(opCtx, _nss, MODE_IX);

        for (size_t doc = 0; doc < docsToBeBatched; doc++) {
            // This test does not call `OpObserver::aboutToDelete`. That method has the
            // side-effect of setting of `documentKey` on the delete for sharding purposes.
            // `OpObserverImpl::onDelete` asserts its existence.
            OplogDeleteEntryArgs args;
            documentKeyDecoration(args).emplace(docsToDelete[doc]["_id"].wrap(), boost::none);
            opCtx->getServiceContext()->getOpObserver()->onDelete(
                opCtx, *autoColl, kUninitializedStmtId, docsToDelete[doc], args);
        }

        wuow.commit();

        // Retrieve the oplog entries. We expect 'docsToBeBatched' oplog entries because of
        // previous iteration of this loop that exercised previous batch sizes.
        std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, docsToBeBatched);
        // Entries in ascending timestamp order, so fetch the last one at the back of the
        // vector.
        auto lastOplogEntry = oplogs.back();
        auto lastOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.back()));

        // The batch consists of an applyOps, whose array contains all deletes issued within the
        // WUOW.
        ASSERT(lastOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
        std::vector<repl::OplogEntry> innerEntries;
        repl::ApplyOps::extractOperationsTo(
            lastOplogEntryParsed, lastOplogEntryParsed.getEntry().toBSON(), &innerEntries);
        ASSERT_EQ(innerEntries.size(), docsToBeBatched);

        for (size_t opIdx = 0; opIdx < docsToBeBatched; opIdx++) {
            const auto innerEntry = innerEntries[opIdx];
            ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
            ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kDelete);
            ASSERT(innerEntry.getNss() == _nss);
            ASSERT(0 == innerEntry.getObject().woCompare(docsToDelete[opIdx]));
        }
    }
}

// Verifies that a WriteUnitOfWork with groupOplogEntries=kGroupForTransaction constisting of an
// insert, an update and a delete replicates as a single applyOps.
TEST_F(BatchedWriteOutputsTest, TestApplyOpsInsertDeleteUpdate) {
    // Setup.
    auto opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    reset(opCtx, _nss);
    reset(opCtx, NamespaceString::kRsOplogNamespace);

    // Start a WUOW with groupOplogEntries=kGroupForTransaction. Verify that initialises the
    // BatchedWriteContext.
    auto& bwc = BatchedWriteContext::get(opCtx);
    ASSERT(!bwc.writesAreBatched());
    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    ASSERT(bwc.writesAreBatched());

    AutoGetCollection autoColl(opCtx, _nss, MODE_IX);

    // (0) Insert
    {
        std::vector<InsertStatement> insert;
        insert.emplace_back(BSON("_id" << 0 << "data"
                                       << "x"));
        opCtx->getServiceContext()->getOpObserver()->onInserts(
            opCtx,
            *autoColl,
            insert.begin(),
            insert.end(),
            /*recordIds=*/{},
            /*fromMigrate=*/std::vector<bool>(insert.size(), false),
            /*defaultFromMigrate=*/false);
    }
    // (1) Delete
    {
        OplogDeleteEntryArgs args;
        auto doc = BSON("_id" << 1);
        documentKeyDecoration(args).emplace(doc, boost::none);
        opCtx->getServiceContext()->getOpObserver()->onDelete(
            opCtx, *autoColl, kUninitializedStmtId, doc, args);
    }
    // (2) Update
    {
        const auto criteria = BSON("_id" << 2);
        const auto preImageDoc = criteria;
        CollectionUpdateArgs collUpdateArgs{preImageDoc};
        collUpdateArgs.criteria = criteria;
        collUpdateArgs.update = BSON("fieldToUpdate"
                                     << "valueToUpdate");
        auto args = OplogUpdateEntryArgs(&collUpdateArgs, *autoColl);
        opCtx->getServiceContext()->getOpObserver()->onUpdate(opCtx, args);
    }

    // And commit the WUOW
    wuow.commit();

    // Retrieve the oplog entries. Implicitly asserts that there's one and only one oplog entry.
    std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, 1);
    auto lastOplogEntry = oplogs.back();
    auto lastOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.back()));

    // The batch consists of an applyOps, whose array contains the three writes issued within
    // the WUOW.
    ASSERT(lastOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        lastOplogEntryParsed, lastOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    ASSERT_EQ(innerEntries.size(), 3);

    {
        const auto innerEntry = innerEntries[0];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT(0 ==
               innerEntry.getObject().woCompare(BSON("_id" << 0 << "data"
                                                           << "x")));
    }
    {
        const auto innerEntry = innerEntries[1];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kDelete);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT(0 == innerEntry.getObject().woCompare(BSON("_id" << 1)));
    }
    {
        const auto innerEntry = innerEntries[2];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kUpdate);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT(0 ==
               innerEntry.getObject().woCompare(BSON("fieldToUpdate"
                                                     << "valueToUpdate")));
    }
}

// Repeat the same test as above, but assert tenantId is included when available
TEST_F(BatchedWriteOutputsTest, TestApplyOpsInsertDeleteUpdateIncludesTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);
    // Setup.
    auto opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    reset(opCtx, _nssWithTid);
    reset(opCtx, NamespaceString::kRsOplogNamespace);

    // Start a WUOW with groupOplogEntries=kGroupForTransaction. Verify that initialises the
    // BatchedWriteContext.
    auto& bwc = BatchedWriteContext::get(opCtx);
    ASSERT(!bwc.writesAreBatched());
    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    ASSERT(bwc.writesAreBatched());

    AutoGetCollection autoColl(opCtx, _nssWithTid, MODE_IX);

    // (0) Insert
    {
        std::vector<InsertStatement> insert;
        insert.emplace_back(BSON("_id" << 0 << "data"
                                       << "x"));
        opCtx->getServiceContext()->getOpObserver()->onInserts(
            opCtx,
            *autoColl,
            insert.begin(),
            insert.end(),
            /*recordIds=*/{},
            /*fromMigrate=*/std::vector<bool>(insert.size(), false),
            /*defaultFromMigrate=*/false);
    }
    // (1) Delete
    {
        OplogDeleteEntryArgs args;
        auto doc = BSON("_id" << 1);
        documentKeyDecoration(args).emplace(doc, boost::none);
        opCtx->getServiceContext()->getOpObserver()->onDelete(
            opCtx, *autoColl, kUninitializedStmtId, doc, args);
    }
    // (2) Update
    {
        const auto criteria = BSON("_id" << 2);
        const auto preImageDoc = criteria;
        CollectionUpdateArgs collUpdateArgs{preImageDoc};
        collUpdateArgs.criteria = criteria;
        collUpdateArgs.update = BSON("fieldToUpdate"
                                     << "valueToUpdate");
        auto args = OplogUpdateEntryArgs(&collUpdateArgs, *autoColl);
        opCtx->getServiceContext()->getOpObserver()->onUpdate(opCtx, args);
    }

    // And commit the WUOW
    wuow.commit();

    // Retrieve the oplog entries. Implicitly asserts that there's one and only one oplog entry.
    std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, 1);
    auto lastOplogEntry = oplogs.back();
    auto lastOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.back()));

    // The batch consists of an applyOps, whose array contains the three writes issued within
    // the WUOW.
    ASSERT(lastOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        lastOplogEntryParsed, lastOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    ASSERT_EQ(innerEntries.size(), 3);

    {
        const auto innerEntry = innerEntries[0];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);

        ASSERT_EQ(innerEntry.getNss(), _nssWithTid);

        ASSERT(innerEntry.getTid().has_value());
        ASSERT(*innerEntry.getTid() == *_nssWithTid.tenantId());
        ASSERT(0 ==
               innerEntry.getObject().woCompare(BSON("_id" << 0 << "data"
                                                           << "x")));
    }

    {
        const auto innerEntry = innerEntries[1];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kDelete);

        ASSERT_EQ(innerEntry.getNss(), _nssWithTid);

        ASSERT(innerEntry.getTid().has_value());
        ASSERT(*innerEntry.getTid() == *_nssWithTid.tenantId());
        ASSERT(0 == innerEntry.getObject().woCompare(BSON("_id" << 1)));
    }

    {
        const auto innerEntry = innerEntries[2];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kUpdate);

        ASSERT_EQ(innerEntry.getNss(), _nssWithTid);

        ASSERT(innerEntry.getTid().has_value());
        ASSERT(*innerEntry.getTid() == *_nssWithTid.tenantId());
        ASSERT(0 ==
               innerEntry.getObject().woCompare(BSON("fieldToUpdate"
                                                     << "valueToUpdate")));
    }
}

// Verifies an empty WUOW doesn't generate an oplog entry.
TEST_F(BatchedWriteOutputsTest, testEmptyWUOW) {
    // Setup.
    auto opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    reset(opCtx, NamespaceString::kRsOplogNamespace);

    // Start and commit an empty WUOW.
    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);
    wuow.commit();

    // The getNOplogEntries call below asserts that the oplog is empty.
    getNOplogEntries(opCtx, 0);
}

// Verifies a large WUOW that is within 16MB of oplog entry succeeds.
TEST_F(BatchedWriteOutputsTest, testWUOWLarge) {
    // Setup.
    auto opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    reset(opCtx, _nss);
    reset(opCtx, NamespaceString::kRsOplogNamespace);

    AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);

    // Delete BatchedWriteOutputsTest::maxDeleteOpsInBatch documents in a single batch, which is the
    // maximum number of docs that can be batched while staying within 16MB of applyOps.
    for (int docId = 0; docId < BatchedWriteOutputsTest::maxDeleteOpsInBatch; docId++) {
        // This test does not call `OpObserver::aboutToDelete`. That method has the side-effect
        // of setting of `documentKey` on the delete for sharding purposes.
        // `OpObserverImpl::onDelete` asserts its existence.
        OplogDeleteEntryArgs args;
        auto doc = BSON("_id" << docId);
        documentKeyDecoration(args).emplace(doc, boost::none);
        opCtx->getServiceContext()->getOpObserver()->onDelete(
            opCtx, *autoColl, kUninitializedStmtId, doc, args);
    }
    wuow.commit();

    // Retrieve the oplog entries, implicitly asserting that there's exactly one entry in the
    // whole oplog.
    std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, 1);
    auto lastOplogEntry = oplogs.back();
    auto lastOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.back()));

    // The batch consists of an applyOps, whose array contains all deletes issued within the
    // WUOW.
    ASSERT(lastOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        lastOplogEntryParsed, lastOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    ASSERT(innerEntries.size() == BatchedWriteOutputsTest::maxDeleteOpsInBatch);
    for (int opIdx = 0; opIdx < BatchedWriteOutputsTest::maxDeleteOpsInBatch; opIdx++) {
        BSONObj o = BSON("_id" << opIdx);
        const auto innerEntry = innerEntries[opIdx];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kDelete);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT(0 == innerEntry.getObject().woCompare(o));
    }
}

// Verifies a WUOW that would result in a an oplog entry >16MB fails with TransactionTooLarge.
TEST_F(BatchedWriteOutputsTest, testWUOWTooLarge) {
    RAIIServerParameterControllerForTest featureFlagController("featureFlagLargeBatchedOperations",
                                                               false);

    // Setup.
    auto opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    reset(opCtx, _nss);
    reset(opCtx, NamespaceString::kRsOplogNamespace);

    AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
    WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForTransaction);

    // Attempt to delete more documents than allowed in a single applyOps batch because it
    // the generated entry exceeds the limit of 16MB for an applyOps entry.
    int approximateDifferenceBetweenBSONObjMaxUserAndInternalSize = 2000;
    for (int docId = 0; docId < BatchedWriteOutputsTest::maxDeleteOpsInBatch +
             approximateDifferenceBetweenBSONObjMaxUserAndInternalSize + 1;
         docId++) {
        // This test does not call `OpObserver::aboutToDelete`. That method has the side-effect
        // of setting of `documentKey` on the delete for sharding purposes.
        // `OpObserverImpl::onDelete` asserts its existence.
        OplogDeleteEntryArgs args;
        auto doc = BSON("_id" << docId);
        documentKeyDecoration(args).emplace(doc, boost::none);
        opCtx->getServiceContext()->getOpObserver()->onDelete(
            opCtx, *autoColl, kUninitializedStmtId, doc, args);
    }

    // This test used to rely on the BSONObjBuilder in packTransactionStatementsForApplyOps()
    // to throw a TransactionTooLarge exception when it is unable to build a BSONObj with the
    // delete operations. Now, we check the limit earlier in OpObserver::onBatchedWriteCommit()
    // using the results from TransactionOperations::getApplyOpsInfo().
    ASSERT_THROWS_CODE(wuow.commit(), DBException, ErrorCodes::TransactionTooLarge);

    // The getNOplogEntries call below asserts that the oplog is empty.
    getNOplogEntries(opCtx, 0);
}

// Verifies that a WriteUnitOfWork with groupOplogEntries=kGroupForPossiblyRetryableOperations
// replicates its writes as a single applyOps. Tests WUOWs batching a range of 1 to 5 inserts
// (inclusive).
TEST_F(BatchedWriteOutputsTest, TestVectoredInsertApplyOpsGrouping) {
    const BSONObj docsToInsert[] = {
        BSON("_id" << 0 << "a" << 10),
        BSON("_id" << 1 << "a" << 11),
        BSON("_id" << 2 << "a" << 12),
        BSON("_id" << 3 << "a" << 13),
        BSON("_id" << 4 << "a" << 14),
    };
    constexpr size_t nDocsToInsert = sizeof(docsToInsert) / sizeof(docsToInsert[0]);

    // Setup.
    auto opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    reset(opCtx, _nss);
    reset(opCtx, NamespaceString::kRsOplogNamespace);

    // Run the test with WUOW's grouping 1 to 5 inserts.
    for (size_t docsToBeBatched = 1; docsToBeBatched <= nDocsToInsert; docsToBeBatched++) {
        // Start a WUOW with groupOplogEntries=kGroupForPossiblyRetryableOperation.
        // Verify that initialises the BatchedWriteContext.
        auto& bwc = BatchedWriteContext::get(opCtx);
        ASSERT(!bwc.writesAreBatched());
        WriteUnitOfWork wuow(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        ASSERT(bwc.writesAreBatched());

        std::vector<InsertStatement> inserts;

        for (size_t i = 0; i < docsToBeBatched; i++) {
            inserts.emplace_back(docsToInsert[i]);
        }

        AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
        opCtx->getServiceContext()->getOpObserver()->onInserts(
            opCtx,
            *autoColl,
            inserts.begin(),
            inserts.end(),
            /*recordIds=*/{},
            /*fromMigrate=*/std::vector<bool>(inserts.size(), false),
            /*defaultFromMigrate=*/false);
        wuow.commit();

        // Retrieve the oplog entries. We expect 'docsToBeBatched' oplog entries because of
        // previous iteration of this loop that exercised previous batch sizes.
        std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, docsToBeBatched);
        // Entries in ascending timestamp order, so fetch the last one at the back of the
        // vector.
        auto lastOplogEntry = oplogs.back();
        auto lastOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.back()));

        // The batch consists of an applyOps, whose array contains all inserts issued within the
        // WUOW.
        ASSERT(lastOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
        // The prevOpTime field should be absent for this applyOps.
        ASSERT(!lastOplogEntryParsed.getPrevWriteOpTimeInTransaction());
        // The multiOpType field should be absent for this applyOps.
        ASSERT(!lastOplogEntryParsed.getMultiOpType());
        // Partial transaction field should not be present
        ASSERT_FALSE(lastOplogEntryParsed.isPartialTransaction());
        // This is not in a retryable session, so we should not have statement IDs.
        ASSERT(lastOplogEntryParsed.getStatementIds().empty());

        // Operations should contain the inserts we did.
        std::vector<repl::OplogEntry> innerEntries;
        repl::ApplyOps::extractOperationsTo(
            lastOplogEntryParsed, lastOplogEntryParsed.getEntry().toBSON(), &innerEntries);
        ASSERT_EQ(innerEntries.size(), docsToBeBatched);
        for (size_t opIdx = 0; opIdx < docsToBeBatched; opIdx++) {
            const auto innerEntry = innerEntries[opIdx];
            ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
            ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);
            ASSERT(innerEntry.getNss() == _nss);
            ASSERT(0 == innerEntry.getObject().woCompare(docsToInsert[opIdx]));
        }
    }
}

TEST_F(BatchedWriteOutputsTest, TestRetryableVectoredInsertApplyOpsGrouping) {
    const BSONObj docsToInsert0[] = {
        BSON("_id" << 0 << "a" << 10),
        BSON("_id" << 1 << "a" << 11),
        BSON("_id" << 2 << "a" << 12),
    };
    constexpr size_t nDocsToInsert0 = sizeof(docsToInsert0) / sizeof(docsToInsert0[0]);

    const BSONObj docsToInsert1[] = {
        BSON("_id" << 3 << "a" << 13),
        BSON("_id" << 4 << "a" << 14),
    };
    constexpr size_t nDocsToInsert1 = sizeof(docsToInsert1) / sizeof(docsToInsert1[0]);

    // Setup.
    auto opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    reset(opCtx, _nss);
    reset(opCtx, NamespaceString::kRsOplogNamespace);

    {
        std::unique_ptr<MongoDSessionCatalog::Session> session;
        beginRetryableWriteWithTxnNumber(opCtx, TxnNumber(1), session);
        AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
        WriteUnitOfWork wuow0(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        std::vector<InsertStatement> inserts0;

        for (size_t i = 0; i < nDocsToInsert0; i++) {
            inserts0.emplace_back(i, docsToInsert0[i]);
        }
        opCtx->getServiceContext()->getOpObserver()->onInserts(
            opCtx,
            *autoColl,
            inserts0.begin(),
            inserts0.end(),
            /*recordIds=*/{},
            /*fromMigrate=*/std::vector<bool>(inserts0.size(), false),
            /*defaultFromMigrate=*/false);
        wuow0.commit();

        WriteUnitOfWork wuow1(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        std::vector<InsertStatement> inserts1;

        for (size_t i = 0; i < nDocsToInsert1; i++) {
            inserts1.emplace_back(i + nDocsToInsert0, docsToInsert1[i]);
        }
        opCtx->getServiceContext()->getOpObserver()->onInserts(
            opCtx,
            *autoColl,
            inserts1.begin(),
            inserts1.end(),
            /*recordIds=*/{},
            /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
            /*defaultFromMigrate=*/false);
        wuow1.commit();
    }

    // Retrieve the oplog entries.  We did two batched WriteUnitsOfWork, so we expect 2.
    std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, 2);
    auto firstOplogEntry = oplogs.front();
    auto firstOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.front()));
    auto lastOplogEntry = oplogs.back();
    auto lastOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.back()));

    // Each batch consists of an applyOps, whose array contains all inserts issued within the
    // WUOW.
    ASSERT(firstOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    ASSERT(lastOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    // The prevOpTime field should be present but null for the first applyOps.
    ASSERT_EQ(firstOplogEntryParsed.getPrevWriteOpTimeInTransaction(), repl::OpTime());
    // The prevOpTime field should be present for the second applyOps, and point to the first
    // one.
    ASSERT_EQ(lastOplogEntryParsed.getPrevWriteOpTimeInTransaction(),
              firstOplogEntryParsed.getOpTime());
    // The multiOpType field should be present for the first applyOps.
    ASSERT_EQ(firstOplogEntryParsed.getMultiOpType(),
              repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);
    // The multiOpType field should be present for the second applyOps.
    ASSERT_EQ(lastOplogEntryParsed.getMultiOpType(),
              repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);
    // Partial transaction fields should not be present
    ASSERT_FALSE(firstOplogEntryParsed.isPartialTransaction());
    ASSERT_FALSE(lastOplogEntryParsed.isPartialTransaction());

    // There should be no statement IDs on the applyOps, only the operations.
    ASSERT(firstOplogEntryParsed.getStatementIds().empty());
    ASSERT(lastOplogEntryParsed.getStatementIds().empty());

    // Operations should contain the inserts we did.
    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        firstOplogEntryParsed, firstOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    ASSERT_EQ(innerEntries.size(), nDocsToInsert0);
    for (size_t opIdx = 0; opIdx < nDocsToInsert0; opIdx++) {
        const auto innerEntry = innerEntries[opIdx];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT_EQ(innerEntry.getStatementIds(), std::vector{StmtId(opIdx)});
        ASSERT_BSONOBJ_EQ(innerEntry.getObject(), docsToInsert0[opIdx]);
    }
    innerEntries.clear();
    repl::ApplyOps::extractOperationsTo(
        lastOplogEntryParsed, lastOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    ASSERT_EQ(innerEntries.size(), nDocsToInsert1);
    for (size_t opIdx = 0; opIdx < nDocsToInsert1; opIdx++) {
        const auto innerEntry = innerEntries[opIdx];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT_EQ(innerEntry.getStatementIds(), std::vector{StmtId(nDocsToInsert0 + opIdx)});
        ASSERT_BSONOBJ_EQ(innerEntry.getObject(), docsToInsert1[opIdx]);
    }
}

// Test to make sure vectored inserts work if the vectored inserts don't fit into an applyOps.  This
// should never happen except in tests which specifically set the batching parameters, but it makes
// the code simpler to assume it does happen, and testing that it works is simpler and more reliable
// than testing that it can't happen.
TEST_F(BatchedWriteOutputsTest, TestRetryableVectoredInsertMultiApplyOpsGrouping) {
    constexpr int kMaxDocsInBatch = 2;
    // This test expects the docsToInsert0 to be two batches long, and docsToInsert1 to fit in one
    // batch.
    RAIIServerParameterControllerForTest batchReducer(
        "maxNumberOfBatchedOperationsInSingleOplogEntry", kMaxDocsInBatch);
    const BSONObj docsToInsert0[] = {
        BSON("_id" << 0 << "a" << 10),
        BSON("_id" << 1 << "a" << 11),
        BSON("_id" << 2 << "a" << 12),
    };
    constexpr size_t nDocsToInsert0 = sizeof(docsToInsert0) / sizeof(docsToInsert0[0]);

    const BSONObj docsToInsert1[] = {
        BSON("_id" << 3 << "a" << 13),
        BSON("_id" << 4 << "a" << 14),
    };
    constexpr size_t nDocsToInsert1 = sizeof(docsToInsert1) / sizeof(docsToInsert1[0]);

    // Setup.
    auto opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    reset(opCtx, _nss);
    reset(opCtx, NamespaceString::kRsOplogNamespace);

    {
        std::unique_ptr<MongoDSessionCatalog::Session> session;
        beginRetryableWriteWithTxnNumber(opCtx, TxnNumber(1), session);
        AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
        WriteUnitOfWork wuow0(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        std::vector<InsertStatement> inserts0;

        for (size_t i = 0; i < nDocsToInsert0; i++) {
            inserts0.emplace_back(i, docsToInsert0[i]);
        }
        opCtx->getServiceContext()->getOpObserver()->onInserts(
            opCtx,
            *autoColl,
            inserts0.begin(),
            inserts0.end(),
            /*recordIds=*/{},
            /*fromMigrate=*/std::vector<bool>(inserts0.size(), false),
            /*defaultFromMigrate=*/false);
        wuow0.commit();

        WriteUnitOfWork wuow1(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        std::vector<InsertStatement> inserts1;

        for (size_t i = 0; i < nDocsToInsert1; i++) {
            inserts1.emplace_back(i + nDocsToInsert0, docsToInsert1[i]);
        }
        opCtx->getServiceContext()->getOpObserver()->onInserts(
            opCtx,
            *autoColl,
            inserts1.begin(),
            inserts1.end(),
            /*recordIds=*/{},
            /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
            /*defaultFromMigrate=*/false);
        wuow1.commit();
    }

    // Retrieve the oplog entries.  We did two batched WriteUnitsOfWork, but the first was too big,
    // so we expect 3.
    std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, 3);
    auto firstOplogEntry = oplogs.front();
    auto firstOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.front()));
    auto middleOplogEntry = oplogs[1];
    auto middleOplogEntryParsed = assertGet(OplogEntry::parse(oplogs[1]));
    auto lastOplogEntry = oplogs.back();
    auto lastOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.back()));

    // Each batch consists of an applyOps, whose array contains all inserts issued within the
    // WUOW.
    ASSERT(firstOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    ASSERT(middleOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    ASSERT(lastOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    // The prevOpTime field should be present but null for the first applyOps.
    ASSERT_EQ(firstOplogEntryParsed.getPrevWriteOpTimeInTransaction(), repl::OpTime());
    // The prevOpTime field should be present for the middle applyOps, and point to the first
    // one.
    ASSERT_EQ(middleOplogEntryParsed.getPrevWriteOpTimeInTransaction(),
              firstOplogEntryParsed.getOpTime());
    // The prevOpTime field should be present for the last applyOps, and point to the middle
    // one.
    ASSERT_EQ(lastOplogEntryParsed.getPrevWriteOpTimeInTransaction(),
              middleOplogEntryParsed.getOpTime());
    // The multiOpType field should be present for the first applyOps.
    ASSERT_EQ(firstOplogEntryParsed.getMultiOpType(),
              repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);
    // The multiOpType field should be present for the second applyOps.
    ASSERT_EQ(middleOplogEntryParsed.getMultiOpType(),
              repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);
    // The multiOpType field should be present for the third applyOps.
    ASSERT_EQ(lastOplogEntryParsed.getMultiOpType(),
              repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);
    // Partial transaction fields should not be present
    ASSERT_FALSE(firstOplogEntryParsed.isPartialTransaction());
    ASSERT_FALSE(middleOplogEntryParsed.isPartialTransaction());
    ASSERT_FALSE(lastOplogEntryParsed.isPartialTransaction());
    // There should be no statement IDs on the applyOps, only the operations.
    ASSERT(firstOplogEntryParsed.getStatementIds().empty());
    ASSERT(middleOplogEntryParsed.getStatementIds().empty());
    ASSERT(lastOplogEntryParsed.getStatementIds().empty());
    // Operations should contain the inserts we did.
    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        firstOplogEntryParsed, firstOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    ASSERT_EQ(innerEntries.size(), kMaxDocsInBatch);
    for (size_t opIdx = 0; opIdx < kMaxDocsInBatch; opIdx++) {
        const auto innerEntry = innerEntries[opIdx];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT_BSONOBJ_EQ(innerEntry.getObject(), docsToInsert0[opIdx]);
    }

    innerEntries.clear();
    repl::ApplyOps::extractOperationsTo(
        middleOplogEntryParsed, middleOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    ASSERT_EQ(innerEntries.size(), nDocsToInsert0 - kMaxDocsInBatch);
    for (size_t opIdx = 0; opIdx < nDocsToInsert0 - kMaxDocsInBatch; opIdx++) {
        const auto innerEntry = innerEntries[opIdx];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT_EQ(innerEntry.getStatementIds(), std::vector{StmtId(kMaxDocsInBatch + opIdx)});
        ASSERT_BSONOBJ_EQ(innerEntry.getObject(), docsToInsert0[kMaxDocsInBatch + opIdx]);
    }

    innerEntries.clear();
    repl::ApplyOps::extractOperationsTo(
        lastOplogEntryParsed, lastOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    for (size_t opIdx = 0; opIdx < nDocsToInsert1; opIdx++) {
        const auto innerEntry = innerEntries[opIdx];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT_EQ(innerEntry.getStatementIds(), std::vector{StmtId(nDocsToInsert0 + opIdx)});
        ASSERT_BSONOBJ_EQ(innerEntry.getObject(), docsToInsert1[opIdx]);
    }
}

// Test to make sure vectored inserts work if the vectored inserts don't fit into an applyOps.  This
// should never happen except in tests which specifically set the batching parameters, but it makes
// the code simpler to assume it does happen, and testing that it works is simpler and more reliable
// than testing that it can't happen.  This tests the non-retryable case, in which case the oplog
// entries should not be linked at all.
TEST_F(BatchedWriteOutputsTest, TestNonRetryableVectoredInsertMultiApplyOpsGrouping) {
    constexpr int kMaxDocsInBatch = 2;
    // This test expects the docsToInsert0 to be two batches long, and docsToInsert1 to fit in one
    // batch.
    RAIIServerParameterControllerForTest batchReducer(
        "maxNumberOfBatchedOperationsInSingleOplogEntry", kMaxDocsInBatch);
    const BSONObj docsToInsert0[] = {
        BSON("_id" << 0 << "a" << 10),
        BSON("_id" << 1 << "a" << 11),
        BSON("_id" << 2 << "a" << 12),
    };
    constexpr size_t nDocsToInsert0 = sizeof(docsToInsert0) / sizeof(docsToInsert0[0]);

    const BSONObj docsToInsert1[] = {
        BSON("_id" << 3 << "a" << 13),
        BSON("_id" << 4 << "a" << 14),
    };
    constexpr size_t nDocsToInsert1 = sizeof(docsToInsert1) / sizeof(docsToInsert1[0]);

    // Setup.
    auto opCtxRaii = cc().makeOperationContext();
    OperationContext* opCtx = opCtxRaii.get();
    reset(opCtx, _nss);
    reset(opCtx, NamespaceString::kRsOplogNamespace);

    {
        AutoGetCollection autoColl(opCtx, _nss, MODE_IX);
        WriteUnitOfWork wuow0(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        std::vector<InsertStatement> inserts0;

        for (size_t i = 0; i < nDocsToInsert0; i++) {
            inserts0.emplace_back(docsToInsert0[i]);
        }
        opCtx->getServiceContext()->getOpObserver()->onInserts(
            opCtx,
            *autoColl,
            inserts0.begin(),
            inserts0.end(),
            /*recordIds=*/{},
            /*fromMigrate=*/std::vector<bool>(inserts0.size(), false),
            /*defaultFromMigrate=*/false);
        wuow0.commit();

        WriteUnitOfWork wuow1(opCtx, WriteUnitOfWork::kGroupForPossiblyRetryableOperations);
        std::vector<InsertStatement> inserts1;

        for (size_t i = 0; i < nDocsToInsert1; i++) {
            inserts1.emplace_back(docsToInsert1[i]);
        }
        opCtx->getServiceContext()->getOpObserver()->onInserts(
            opCtx,
            *autoColl,
            inserts1.begin(),
            inserts1.end(),
            /*recordIds=*/{},
            /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
            /*defaultFromMigrate=*/false);
        wuow1.commit();
    }

    // Retrieve the oplog entries.  We did two batched WriteUnitsOfWork, but the first was too big,
    // so we expect 3.
    std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, 3);
    auto firstOplogEntry = oplogs.front();
    auto firstOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.front()));
    auto middleOplogEntry = oplogs[1];
    auto middleOplogEntryParsed = assertGet(OplogEntry::parse(oplogs[1]));
    auto lastOplogEntry = oplogs.back();
    auto lastOplogEntryParsed = assertGet(OplogEntry::parse(oplogs.back()));

    // Each batch consists of an applyOps, whose array contains all inserts issued within the
    // WUOW.
    ASSERT(firstOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    ASSERT(middleOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    ASSERT(lastOplogEntryParsed.getCommandType() == OplogEntry::CommandType::kApplyOps);
    // The prevOpTime field should be absent for all three entries.
    ASSERT_FALSE(firstOplogEntryParsed.getPrevWriteOpTimeInTransaction());
    ASSERT_FALSE(middleOplogEntryParsed.getPrevWriteOpTimeInTransaction());
    ASSERT_FALSE(lastOplogEntryParsed.getPrevWriteOpTimeInTransaction());
    // The multiOpType field should be absent for all entries.
    ASSERT_FALSE(firstOplogEntryParsed.getMultiOpType());
    // The multiOpType field should be present for the second applyOps.
    ASSERT_FALSE(middleOplogEntryParsed.getMultiOpType());
    // The multiOpType field should be present for the third applyOps.
    ASSERT_FALSE(lastOplogEntryParsed.getMultiOpType());
    // Partial transaction fields should not be present
    ASSERT_FALSE(firstOplogEntryParsed.isPartialTransaction());
    ASSERT_FALSE(middleOplogEntryParsed.isPartialTransaction());
    ASSERT_FALSE(lastOplogEntryParsed.isPartialTransaction());
    // There should be no statement IDs on the applyOps, only the operations.
    ASSERT(firstOplogEntryParsed.getStatementIds().empty());
    ASSERT(middleOplogEntryParsed.getStatementIds().empty());
    ASSERT(lastOplogEntryParsed.getStatementIds().empty());
    // Operations should contain the inserts we did.
    std::vector<repl::OplogEntry> innerEntries;
    repl::ApplyOps::extractOperationsTo(
        firstOplogEntryParsed, firstOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    ASSERT_EQ(innerEntries.size(), kMaxDocsInBatch);
    for (size_t opIdx = 0; opIdx < kMaxDocsInBatch; opIdx++) {
        const auto innerEntry = innerEntries[opIdx];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT_BSONOBJ_EQ(innerEntry.getObject(), docsToInsert0[opIdx]);
    }

    innerEntries.clear();
    repl::ApplyOps::extractOperationsTo(
        middleOplogEntryParsed, middleOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    ASSERT_EQ(innerEntries.size(), nDocsToInsert0 - kMaxDocsInBatch);
    for (size_t opIdx = 0; opIdx < nDocsToInsert0 - kMaxDocsInBatch; opIdx++) {
        const auto innerEntry = innerEntries[opIdx];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT(innerEntry.getStatementIds().empty());
        ASSERT_BSONOBJ_EQ(innerEntry.getObject(), docsToInsert0[kMaxDocsInBatch + opIdx]);
    }

    innerEntries.clear();
    repl::ApplyOps::extractOperationsTo(
        lastOplogEntryParsed, lastOplogEntryParsed.getEntry().toBSON(), &innerEntries);
    for (size_t opIdx = 0; opIdx < nDocsToInsert1; opIdx++) {
        const auto innerEntry = innerEntries[opIdx];
        ASSERT(innerEntry.getCommandType() == OplogEntry::CommandType::kNotCommand);
        ASSERT(innerEntry.getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT(innerEntry.getNss() == _nss);
        ASSERT(innerEntry.getStatementIds().empty());
        ASSERT_BSONOBJ_EQ(innerEntry.getObject(), docsToInsert1[opIdx]);
    }
}

class OnDeleteOutputsTest : public OpObserverTest {

protected:
    void setUp() override {
        OpObserverTest::setUp();
        auto opCtx = cc().makeOperationContext();
        reset(opCtx.get(), _nss, _uuid);
    }

    void logTestCase(const DeleteTestCase& testCase) {
        LOGV2(5739905,
              "DeleteTestCase",
              "ChangeStreamPreAndPostImagesEnabled"_attr = testCase.changeStreamImagesEnabled,
              "RetryableFindAndModifyLocation"_attr =
                  testCase.getRetryableFindAndModifyLocationStr(),
              "ExpectedOplogEntries"_attr = testCase.numOutputOplogs);
    }

    void initializeOplogDeleteEntryArgs(OperationContext* opCtx,
                                        const DeleteTestCase& testCase,
                                        OplogDeleteEntryArgs* deleteArgs) {
        deleteArgs->changeStreamPreAndPostImagesEnabledForCollection =
            testCase.changeStreamImagesEnabled;

        switch (testCase.retryableOptions) {
            case kNotRetryable:
                deleteArgs->retryableFindAndModifyLocation = kNotRetryable;
                break;
            case kRecordInSideCollection:
                deleteArgs->retryableFindAndModifyLocation = kRecordInSideCollection;
                break;
        }
    }

    void checkSideCollectionIfNeeded(
        OperationContext* opCtx,
        const DeleteTestCase& testCase,
        const OplogDeleteEntryArgs& deleteArgs,
        const std::vector<BSONObj>& oplogs,
        const OplogEntry& deleteOplogEntry,
        const boost::optional<OplogEntry> applyOpsOplogEntry = boost::none) {
        bool didWriteInSideCollection =
            deleteArgs.retryableFindAndModifyLocation == kRecordInSideCollection;
        if (didWriteInSideCollection) {
            repl::ImageEntry imageEntry =
                getImageEntryFromSideCollection(opCtx, *deleteOplogEntry.getSessionId());
            ASSERT(imageEntry.getImageKind() == deleteOplogEntry.getNeedsRetryImage());
            if (applyOpsOplogEntry) {
                ASSERT_FALSE(applyOpsOplogEntry->getNeedsRetryImage());
            }
            ASSERT(imageEntry.getImageKind() == repl::RetryImageEnum::kPreImage);
            ASSERT_BSONOBJ_EQ(_deletedDoc, imageEntry.getImage());

            // If 'deleteOplogEntry' has opTime T, opTime T-1 must be reserved for potential
            // forged noop oplog entry for the preImage written to the side collection.
            const Timestamp forgeNoopTimestamp = deleteOplogEntry.getTimestamp() - 1;
            ASSERT_FALSE(findByTimestamp(oplogs, forgeNoopTimestamp));
        } else {
            ASSERT_FALSE(deleteOplogEntry.getNeedsRetryImage());
            if (deleteOplogEntry.getSessionId()) {
                ASSERT_FALSE(
                    didWriteImageEntryToSideCollection(opCtx, *deleteOplogEntry.getSessionId()));
            } else {
                // Session id is missing only for non-retryable option.
                ASSERT(testCase.retryableOptions == kNotRetryable);
            }
        }
    }

    void checkChangeStreamImagesIfNeeded(OperationContext* opCtx,
                                         const DeleteTestCase& testCase,
                                         const OplogDeleteEntryArgs& deleteArgs,
                                         const OplogEntry& deleteOplogEntry) {
        const Timestamp preImageOpTime = deleteOplogEntry.getOpTime().getTimestamp();
        ChangeStreamPreImageId preImageId(_uuid, preImageOpTime, 0);
        if (deleteArgs.changeStreamPreAndPostImagesEnabledForCollection) {
            BSONObj container;
            ChangeStreamPreImage preImage = getChangeStreamPreImage(opCtx, preImageId, &container);
            ASSERT_BSONOBJ_EQ(_deletedDoc, preImage.getPreImage());
            ASSERT_EQ(deleteOplogEntry.getWallClockTime(), preImage.getOperationTime());
        } else {
            ASSERT_FALSE(didWriteDeletedDocToPreImagesCollection(opCtx, preImageId));
        }
    }

    std::vector<DeleteTestCase> _cases{{kChangeStreamImagesDisabled, kNotRetryable, 1},
                                       {kChangeStreamImagesDisabled, kRecordInSideCollection, 1},
                                       {kChangeStreamImagesEnabled, kNotRetryable, 1},
                                       {kChangeStreamImagesEnabled, kRecordInSideCollection, 1}};

    const NamespaceString _nss =
        NamespaceString::createNamespaceString_forTest(boost::none, "test", "coll");
    const UUID _uuid = UUID::gen();
    const BSONObj _deletedDoc = BSON("_id" << 0 << "valuePriorToDelete"
                                           << "marvelous");
};

TEST_F(OnDeleteOutputsTest, TestNonTransactionFundamentalOnDeleteOutputs) {
    // Create a registry that registers the OpObserverImpl, FindAndModifyImagesOpObserver, and
    // ChangeStreamPreImagesOpObserver.
    // These OpObservers work together to ensure that images for retryable findAndModify and
    // change streams are written correctly to the respective side collections.
    // It falls into cases where `ReservedTimes` is expected to be instantiated. Due to strong
    // encapsulation, we use the registry that managers the `ReservedTimes` on our behalf.
    OpObserverRegistry opObserver;
    opObserver.addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    opObserver.addObserver(std::make_unique<FindAndModifyImagesOpObserver>());
    opObserver.addObserver(std::make_unique<ChangeStreamPreImagesOpObserver>());

    for (std::size_t testIdx = 0; testIdx < _cases.size(); ++testIdx) {
        const auto& testCase = _cases[testIdx];
        logTestCase(testCase);

        auto opCtxRaii = cc().makeOperationContext();
        OperationContext* opCtx = opCtxRaii.get();

        // Phase 1: Clearing any state and setting up fixtures/the delete call.
        resetOplogAndTransactions(opCtx);

        std::unique_ptr<MongoDSessionCatalog::Session> contextSession;
        if (testCase.isRetryable()) {
            beginRetryableWriteWithTxnNumber(opCtx, testIdx, contextSession);
        }

        // Phase 2: Call the code we're testing.
        OplogDeleteEntryArgs deleteEntryArgs;
        initializeOplogDeleteEntryArgs(opCtx, testCase, &deleteEntryArgs);

        WriteUnitOfWork wuow(opCtx);
        AutoGetCollection locks(opCtx, _nss, MODE_IX);
        // This test does not call `OpObserver::aboutToDelete`. That method has the side-effect
        // of setting of `documentKey` on the delete for sharding purposes.
        // `OpObserverImpl::onDelete` asserts its existence.
        documentKeyDecoration(deleteEntryArgs).emplace(_deletedDoc["_id"].wrap(), boost::none);
        opObserver.onDelete(opCtx,
                            *locks,
                            testCase.isRetryable() ? 1 : kUninitializedStmtId,
                            _deletedDoc,
                            deleteEntryArgs);
        wuow.commit();

        // Phase 3: Analyze the results:
        // This `getNOplogEntries` also asserts that all oplogs are retrieved.
        std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, testCase.numOutputOplogs);
        // Entries are returned in ascending timestamp order.
        auto deleteOplogEntry = assertGet(OplogEntry::parse(oplogs.back()));
        checkSideCollectionIfNeeded(opCtx, testCase, deleteEntryArgs, oplogs, deleteOplogEntry);
        checkChangeStreamImagesIfNeeded(opCtx, testCase, deleteEntryArgs, deleteOplogEntry);
    }
}

TEST_F(OnDeleteOutputsTest, TestTransactionFundamentalOnDeleteOutputs) {
    // Create a registry that registers the OpObserverImpl, FindAndModifyImagesOpObserver, and
    // ChangeStreamPreImagesOpObserver.
    // These OpObservers work together to ensure that images for retryable findAndModify and
    // change streams are written correctly to the respective side collections.
    // It falls into cases where `ReservedTimes` is expected to be instantiated. Due to strong
    // encapsulation, we use the registry that managers the `ReservedTimes` on our behalf.
    OpObserverRegistry opObserver;
    opObserver.addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerImpl>()));
    opObserver.addObserver(std::make_unique<FindAndModifyImagesOpObserver>());
    opObserver.addObserver(std::make_unique<ChangeStreamPreImagesOpObserver>());

    for (std::size_t testIdx = 0; testIdx < _cases.size(); ++testIdx) {
        const auto& testCase = _cases[testIdx];
        logTestCase(testCase);

        auto opCtxRaii = cc().makeOperationContext();
        OperationContext* opCtx = opCtxRaii.get();

        // Phase 1: Clearing any state and setting up fixtures/the delete call.
        resetOplogAndTransactions(opCtx);

        std::unique_ptr<MongoDSessionCatalog::Session> contextSession;
        if (testCase.isRetryable()) {
            beginRetryableInternalTransactionWithTxnNumber(opCtx, testIdx, contextSession);
        } else {
            beginNonRetryableTransactionWithTxnNumber(opCtx, testIdx, contextSession);
        }

        // Phase 2: Call the code we're testing.
        OplogDeleteEntryArgs deleteEntryArgs;
        initializeOplogDeleteEntryArgs(opCtx, testCase, &deleteEntryArgs);
        const auto stmtId = testCase.isRetryable() ? 1 : kUninitializedStmtId;

        WriteUnitOfWork wuow(opCtx);
        AutoGetCollection locks(opCtx, _nss, MODE_IX);
        // This test does not call `OpObserver::aboutToDelete`. That method has the side-effect
        // of setting of `documentKey` on the delete for sharding purposes.
        // `OpObserverImpl::onDelete` asserts its existence.
        documentKeyDecoration(deleteEntryArgs).emplace(_deletedDoc["_id"].wrap(), boost::none);
        opObserver.onDelete(opCtx, *locks, stmtId, _deletedDoc, deleteEntryArgs);
        commitUnpreparedTransaction<OpObserverRegistry>(opCtx, opObserver);
        wuow.commit();

        // Phase 3: Analyze the results:
        // This `getNOplogEntries` also asserts that all oplogs are retrieved.
        std::vector<BSONObj> oplogs = getNOplogEntries(opCtx, testCase.numOutputOplogs);
        // Entries are returned in ascending timestamp order.
        auto applyOpsOplogEntry = assertGet(OplogEntry::parse(oplogs.back()));
        auto deleteOplogEntry = getInnerEntryFromApplyOpsOplogEntry(applyOpsOplogEntry);
        checkSideCollectionIfNeeded(
            opCtx, testCase, deleteEntryArgs, oplogs, deleteOplogEntry, applyOpsOplogEntry);
        checkChangeStreamImagesIfNeeded(opCtx, testCase, deleteEntryArgs, deleteOplogEntry);
    }
}

class OpObserverMultiEntryTransactionTest : public OpObserverTransactionTest {
    void setUp() override {
        _prevPackingLimit = gMaxNumberOfTransactionOperationsInSingleOplogEntry;
        gMaxNumberOfTransactionOperationsInSingleOplogEntry = 1;
        OpObserverTransactionTest::setUp();
    }

    void tearDown() override {
        OpObserverTransactionTest::tearDown();
        gMaxNumberOfTransactionOperationsInSingleOplogEntry = _prevPackingLimit;
    }

private:
    int _prevPackingLimit;
};

TEST_F(OpObserverMultiEntryTransactionTest, TransactionSingleStatementTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    std::vector<InsertStatement> inserts;
    inserts.emplace_back(0, BSON("_id" << 0 << "a" << std::string(BSONObjMaxUserSize, 'a')));

    AutoGetCollection autoColl(opCtx(), nss, MODE_IX);
    WriteUnitOfWork wuow(opCtx());
    opObserver().onInserts(opCtx(),
                           *autoColl,
                           inserts.begin(),
                           inserts.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts.size(), false),
                           /*defaultFromMigrate=*/false);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    auto oplogEntryObj = getNOplogEntries(opCtx(), 1)[0];
    checkSessionAndTransactionFields(oplogEntryObj);
    auto oplogEntry = assertGet(OplogEntry::parse(oplogEntryObj));
    ASSERT(!oplogEntry.shouldPrepare());
    ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(repl::OpTime(), *oplogEntry.getPrevWriteOpTimeInTransaction());

    // The implicit commit oplog entry.
    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON(
                              "op"
                              << "i"
                              << "ns" << nss.toString_forTest() << "ui" << uuid << "o"
                              << BSON("_id" << 0 << "a" << std::string(BSONObjMaxUserSize, 'a'))
                              << "o2" << BSON("_id" << 0))));
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntry.getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalInsertTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0, BSON("_id" << 0));
    inserts1.emplace_back(1, BSON("_id" << 1));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0, BSON("_id" << 2));
    inserts2.emplace_back(1, BSON("_id" << 3));
    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onInserts(opCtx(),
                           *autoColl1,
                           inserts1.begin(),
                           inserts1.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
                           /*defaultFromMigrate=*/false);
    opObserver().onInserts(opCtx(),
                           *autoColl2,
                           inserts2.begin(),
                           inserts2.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts2.size(), false),
                           /*defaultFromMigrate=*/false);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 4);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }
    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss1.toString_forTest() << "ui" << uuid1
                                           << "o" << BSON("_id" << 0) << "o2" << BSON("_id" << 0)))
                        << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss1.toString_forTest() << "ui" << uuid1
                                           << "o" << BSON("_id" << 1) << "o2" << BSON("_id" << 1)))
                        << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());

    oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss2.toString_forTest() << "ui" << uuid2
                                           << "o" << BSON("_id" << 2) << "o2" << BSON("_id" << 2)))
                        << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[2].getObject());

    // This should be the implicit commit oplog entry, indicated by the absence of the
    // 'partialTxn' field.
    oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss2.toString_forTest() << "ui" << uuid2
                                           << "o" << BSON("_id" << 3) << "o2" << BSON("_id" << 3)))
                        << "count" << 4);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[3].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalUpdateTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "update");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    const auto criteria1 = BSON("_id" << 0);
    const auto preImageDoc1 = criteria1;
    CollectionUpdateArgs updateArgs1{preImageDoc1};
    updateArgs1.criteria = criteria1;
    updateArgs1.stmtIds = {0};
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    OplogUpdateEntryArgs update1(&updateArgs1, *autoColl1);

    const auto criteria2 = BSON("_id" << 1);
    const auto preImageDoc2 = criteria2;
    CollectionUpdateArgs updateArgs2{preImageDoc2};
    updateArgs2.criteria = criteria2;
    updateArgs2.stmtIds = {1};
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    OplogUpdateEntryArgs update2(&updateArgs2, *autoColl2);

    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected = BSON(
        "applyOps" << BSON_ARRAY(BSON("op"
                                      << "u"
                                      << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                      << BSON("$set" << BSON("data"
                                                             << "x"))
                                      << "o2" << BSON("_id" << 0)))
                   << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    // This should be the implicit commit oplog entry, indicated by the absence of the
    // 'partialTxn' field.
    oExpected = BSON(
        "applyOps" << BSON_ARRAY(BSON("op"
                                      << "u"
                                      << "ns" << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                      << BSON("$set" << BSON("data"
                                                             << "y"))
                                      << "o2" << BSON("_id" << 1)))
                   << "count" << 2);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalDeleteTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "delete");

    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    OplogDeleteEntryArgs args;

    auto doc1 = BSON("_id" << 0 << "data"
                           << "x");
    opObserver().aboutToDelete(opCtx(), *autoColl1, doc1, &args);
    opObserver().onDelete(opCtx(), *autoColl1, 0, doc1, args);

    auto doc2 = BSON("_id" << 1 << "data"
                           << "y");
    opObserver().aboutToDelete(opCtx(), *autoColl2, doc2, &args);
    opObserver().onDelete(opCtx(), *autoColl2, 0, doc2, args);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "d"
                                                        << "ns" << nss1.toString_forTest() << "ui"
                                                        << uuid1 << "o" << BSON("_id" << 0)))
                                     << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    // This should be the implicit commit oplog entry, indicated by the absence of the
    // 'partialTxn' field.
    oExpected = oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "d"
                                           << "ns" << nss2.toString_forTest() << "ui" << uuid2
                                           << "o" << BSON("_id" << 1)))
                        << "count" << 2);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalInsertPrepareTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0, BSON("_id" << 0));
    inserts1.emplace_back(1, BSON("_id" << 1));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0, BSON("_id" << 2));
    inserts2.emplace_back(1, BSON("_id" << 3));

    opObserver().onInserts(opCtx(),
                           *autoColl1,
                           inserts1.begin(),
                           inserts1.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
                           /*defaultFromMigrate=*/false);
    opObserver().onInserts(opCtx(),
                           *autoColl2,
                           inserts2.begin(),
                           inserts2.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts2.size(), false),
                           /*defaultFromMigrate=*/false);

    auto reservedSlots = reserveOpTimesInSideTransaction(opCtx(), 4);
    auto prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    prepareTransaction(reservedSlots, prepareOpTime);

    ASSERT_EQ(prepareOpTime.getTimestamp(),
              shard_role_details::getRecoveryUnit(opCtx())->getPrepareTimestamp());
    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());

    txnParticipant.stashTransactionResources(opCtx());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 4);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss1.toString_forTest() << "ui" << uuid1
                                           << "o" << BSON("_id" << 0) << "o2" << BSON("_id" << 0)))
                        << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss1.toString_forTest() << "ui" << uuid1
                                           << "o" << BSON("_id" << 1) << "o2" << BSON("_id" << 1)))
                        << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());

    oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss2.toString_forTest() << "ui" << uuid2
                                           << "o" << BSON("_id" << 2) << "o2" << BSON("_id" << 2)))
                        << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[2].getObject());

    oExpected =
        BSON("applyOps" << BSON_ARRAY(BSON("op"
                                           << "i"
                                           << "ns" << nss2.toString_forTest() << "ui" << uuid2
                                           << "o" << BSON("_id" << 3) << "o2" << BSON("_id" << 3)))
                        << "prepare" << true << "count" << 4);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[3].getObject());

    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalUpdatePrepareTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "update");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);

    const auto criteria1 = BSON("_id" << 0);
    const auto preImageDoc1 = criteria1;
    CollectionUpdateArgs updateArgs1{preImageDoc1};
    updateArgs1.criteria = criteria1;
    updateArgs1.stmtIds = {0};
    updateArgs1.updatedDoc = BSON("_id" << 0 << "data"
                                        << "x");
    updateArgs1.update = BSON("$set" << BSON("data"
                                             << "x"));
    OplogUpdateEntryArgs update1(&updateArgs1, *autoColl1);

    const auto criteria2 = BSON("_id" << 1);
    const auto preImageDoc2 = criteria2;
    CollectionUpdateArgs updateArgs2{preImageDoc2};
    updateArgs2.criteria = criteria2;
    updateArgs2.stmtIds = {1};
    updateArgs2.updatedDoc = BSON("_id" << 1 << "data"
                                        << "y");
    updateArgs2.update = BSON("$set" << BSON("data"
                                             << "y"));
    OplogUpdateEntryArgs update2(&updateArgs2, *autoColl2);

    opObserver().onUpdate(opCtx(), update1);
    opObserver().onUpdate(opCtx(), update2);

    auto reservedSlots = reserveOpTimesInSideTransaction(opCtx(), 2);
    auto prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    prepareTransaction(reservedSlots, prepareOpTime);

    ASSERT_EQ(prepareOpTime.getTimestamp(),
              shard_role_details::getRecoveryUnit(opCtx())->getPrepareTimestamp());
    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());

    txnParticipant.stashTransactionResources(opCtx());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected = BSON(
        "applyOps" << BSON_ARRAY(BSON("op"
                                      << "u"
                                      << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                      << BSON("$set" << BSON("data"
                                                             << "x"))
                                      << "o2" << BSON("_id" << 0)))
                   << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    oExpected = BSON(
        "applyOps" << BSON_ARRAY(BSON("op"
                                      << "u"
                                      << "ns" << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                      << BSON("$set" << BSON("data"
                                                             << "y"))
                                      << "o2" << BSON("_id" << 1)))
                   << "prepare" << true << "count" << 2);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());

    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverMultiEntryTransactionTest, TransactionalDeletePrepareTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "delete");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    OplogDeleteEntryArgs args;

    auto doc1 = BSON("_id" << 0 << "data"
                           << "x");
    opObserver().aboutToDelete(opCtx(), *autoColl1, doc1, &args);
    opObserver().onDelete(opCtx(), *autoColl1, 0, doc1, args);

    auto doc2 = BSON("_id" << 1 << "data"
                           << "y");
    opObserver().aboutToDelete(opCtx(), *autoColl2, doc2, &args);
    opObserver().onDelete(opCtx(), *autoColl2, 0, doc2, args);

    auto reservedSlots = reserveOpTimesInSideTransaction(opCtx(), 2);
    auto prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    prepareTransaction(reservedSlots, prepareOpTime);

    ASSERT_EQ(prepareOpTime.getTimestamp(),
              shard_role_details::getRecoveryUnit(opCtx())->getPrepareTimestamp());
    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());

    txnParticipant.stashTransactionResources(opCtx());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                        << "d"
                                                        << "ns" << nss1.toString_forTest() << "ui"
                                                        << uuid1 << "o" << BSON("_id" << 0)))
                                     << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    oExpected = BSON("applyOps" << BSON_ARRAY(BSON("op"
                                                   << "d"
                                                   << "ns" << nss2.toString_forTest() << "ui"
                                                   << uuid2 << "o" << BSON("_id" << 1)))
                                << "prepare" << true << "count" << 2);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());

    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverMultiEntryTransactionTest, CommitPreparedTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));

    opObserver().onInserts(opCtx(),
                           *autoColl1,
                           inserts1.begin(),
                           inserts1.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
                           /*defaultFromMigrate=*/false);

    auto reservedSlots = reserveOpTimesInSideTransaction(opCtx(), 2);
    auto prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    prepareTransaction(reservedSlots, prepareOpTime);

    txnParticipant.stashTransactionResources(opCtx());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);

    const auto insertEntry = assertGet(OplogEntry::parse(oplogEntryObjs[0]));
    ASSERT_TRUE(insertEntry.getOpType() == repl::OpTypeEnum::kCommand);
    ASSERT_TRUE(insertEntry.getCommandType() == OplogEntry::CommandType::kApplyOps);

    // This should be the implicit prepare entry.
    const auto prepareEntry = assertGet(OplogEntry::parse(oplogEntryObjs[1]));
    ASSERT_TRUE(prepareEntry.getOpType() == repl::OpTypeEnum::kCommand);
    ASSERT_TRUE(prepareEntry.getCommandType() == OplogEntry::CommandType::kApplyOps);
    ASSERT_EQ(prepareEntry.getObject()["prepare"].boolean(), true);

    const auto startOpTime = insertEntry.getOpTime();

    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");
    const auto prepareTimestamp = prepareOpTime.getTimestamp();
    ASSERT_EQ(prepareTimestamp,
              shard_role_details::getRecoveryUnit(opCtx())->getPrepareTimestamp());

    // Reserve oplog entry for the commit oplog entry.
    OplogSlot commitSlot = reserveOpTimeInSideTransaction(opCtx());

    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());
    txnParticipant.stashTransactionResources(opCtx());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    assertTxnRecordStartOpTime(startOpTime);
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // Mimic committing the transaction.
    shard_role_details::setWriteUnitOfWork(opCtx(), nullptr);
    shard_role_details::getLocker(opCtx())->unsetMaxLockTimeout();

    // commitTimestamp must be greater than the prepareTimestamp.
    auto commitTimestamp = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onPreparedTransactionCommit(
            opCtx(),
            commitSlot,
            commitTimestamp,
            txnParticipant.retrieveCompletedTransactionOperations(opCtx())
                ->getOperationsForOpObserver());
    }
    oplogEntryObjs = getNOplogEntries(opCtx(), 3);
    const auto commitOplogObj = oplogEntryObjs.back();
    checkSessionAndTransactionFields(commitOplogObj);
    auto commitEntry = assertGet(OplogEntry::parse(commitOplogObj));
    auto o = commitEntry.getObject();
    auto oExpected = BSON("commitTransaction" << 1 << "commitTimestamp" << commitTimestamp);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_TRUE(commitEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(*commitEntry.getPrevWriteOpTimeInTransaction(), prepareEntry.getOpTime());

    assertTxnRecord(txnNum(), commitSlot, DurableTxnStateEnum::kCommitted);
    // startTimestamp should no longer be set once the transaction has been committed.
    assertTxnRecordStartOpTime(boost::none);
}

TEST_F(OpObserverMultiEntryTransactionTest, AbortPreparedTest) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));

    opObserver().onInserts(opCtx(),
                           *autoColl1,
                           inserts1.begin(),
                           inserts1.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
                           /*defaultFromMigrate=*/false);

    auto reservedSlots = reserveOpTimesInSideTransaction(opCtx(), 1);
    auto prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    prepareTransaction(reservedSlots, prepareOpTime);

    txnParticipant.stashTransactionResources(opCtx());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 1);

    const auto insertEntry = assertGet(OplogEntry::parse(oplogEntryObjs[0]));
    ASSERT_TRUE(insertEntry.getOpType() == repl::OpTypeEnum::kCommand);
    ASSERT_TRUE(insertEntry.getCommandType() == OplogEntry::CommandType::kApplyOps);
    const auto startOpTime = insertEntry.getOpTime();

    const auto prepareTimestamp = prepareOpTime.getTimestamp();

    const auto prepareEntry = insertEntry;
    ASSERT_EQ(prepareEntry.getObject()["prepare"].boolean(), true);

    // Reserve oplog entry for the abort oplog entry.
    OplogSlot abortSlot = reserveOpTimeInSideTransaction(opCtx());

    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    assertTxnRecordStartOpTime(startOpTime);
    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
    ASSERT_EQ(prepareTimestamp,
              shard_role_details::getRecoveryUnit(opCtx())->getPrepareTimestamp());

    // Mimic aborting the transaction by resetting the WUOW.
    shard_role_details::setWriteUnitOfWork(opCtx(), nullptr);
    shard_role_details::getLocker(opCtx())->unsetMaxLockTimeout();
    {
        Lock::GlobalLock lk(opCtx(), MODE_IX);
        opObserver().onTransactionAbort(opCtx(), abortSlot);
    }
    txnParticipant.transitionToAbortedWithPrepareforTest(opCtx());

    txnParticipant.stashTransactionResources(opCtx());
    oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    auto abortOplogObj = oplogEntryObjs.back();
    checkSessionAndTransactionFields(abortOplogObj);
    auto abortEntry = assertGet(OplogEntry::parse(abortOplogObj));
    auto o = abortEntry.getObject();
    auto oExpected = BSON("abortTransaction" << 1);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_TRUE(abortEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(*abortEntry.getPrevWriteOpTimeInTransaction(), prepareEntry.getOpTime());

    assertTxnRecord(txnNum(), abortSlot, DurableTxnStateEnum::kAborted);
    // startOpTime should no longer be set once a transaction has been aborted.
    assertTxnRecordStartOpTime(boost::none);
}

TEST_F(OpObserverMultiEntryTransactionTest, UnpreparedTransactionPackingTest) {
    gMaxNumberOfTransactionOperationsInSingleOplogEntry = std::numeric_limits<int>::max();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0, BSON("_id" << 0));
    inserts1.emplace_back(1, BSON("_id" << 1));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0, BSON("_id" << 2));
    inserts2.emplace_back(1, BSON("_id" << 3));
    WriteUnitOfWork wuow(opCtx());
    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onInserts(opCtx(),
                           *autoColl1,
                           inserts1.begin(),
                           inserts1.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
                           /*defaultFromMigrate=*/false);
    opObserver().onInserts(opCtx(),
                           *autoColl2,
                           inserts2.begin(),
                           inserts2.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts2.size(), false),
                           /*defaultFromMigrate=*/false);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 1);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }
    auto oExpected = BSON(
        "applyOps" << BSON_ARRAY(BSON("op"
                                      << "i"
                                      << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                      << BSON("_id" << 0) << "o2" << BSON("_id" << 0))
                                 << BSON("op"
                                         << "i"
                                         << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                         << BSON("_id" << 1) << "o2" << BSON("_id" << 1))
                                 << BSON("op"
                                         << "i"
                                         << "ns" << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                         << BSON("_id" << 2) << "o2" << BSON("_id" << 2))
                                 << BSON("op"
                                         << "i"
                                         << "ns" << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                         << BSON("_id" << 3) << "o2" << BSON("_id" << 3))));
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());
}

TEST_F(OpObserverMultiEntryTransactionTest, PreparedTransactionPackingTest) {
    gMaxNumberOfTransactionOperationsInSingleOplogEntry = std::numeric_limits<int>::max();

    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");
    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0, BSON("_id" << 0));
    inserts1.emplace_back(1, BSON("_id" << 1));
    std::vector<InsertStatement> inserts2;
    inserts2.emplace_back(0, BSON("_id" << 2));
    inserts2.emplace_back(1, BSON("_id" << 3));

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);
    AutoGetCollection autoColl2(opCtx(), nss2, MODE_IX);
    opObserver().onInserts(opCtx(),
                           *autoColl1,
                           inserts1.begin(),
                           inserts1.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
                           /*defaultFromMigrate=*/false);
    opObserver().onInserts(opCtx(),
                           *autoColl2,
                           inserts2.begin(),
                           inserts2.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts2.size(), false),
                           /*defaultFromMigrate=*/false);

    auto reservedSlots = reserveOpTimesInSideTransaction(opCtx(), 4);
    auto prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    prepareTransaction(reservedSlots, prepareOpTime);

    txnParticipant.stashTransactionResources(opCtx());
    auto oplogEntryObj = getSingleOplogEntry(opCtx());
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    checkSessionAndTransactionFields(oplogEntryObj);
    oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
    const auto& oplogEntry = oplogEntries.back();
    ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
    expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};

    auto oExpected = BSON(
        "applyOps" << BSON_ARRAY(BSON("op"
                                      << "i"
                                      << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                      << BSON("_id" << 0) << "o2" << BSON("_id" << 0))
                                 << BSON("op"
                                         << "i"
                                         << "ns" << nss1.toString_forTest() << "ui" << uuid1 << "o"
                                         << BSON("_id" << 1) << "o2" << BSON("_id" << 1))
                                 << BSON("op"
                                         << "i"
                                         << "ns" << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                         << BSON("_id" << 2) << "o2" << BSON("_id" << 2))
                                 << BSON("op"
                                         << "i"
                                         << "ns" << nss2.toString_forTest() << "ui" << uuid2 << "o"
                                         << BSON("_id" << 3) << "o2" << BSON("_id" << 3)))
                   << "prepare" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    txnParticipant.unstashTransactionResources(opCtx(), "abortTransaction");
}

TEST_F(OpObserverMultiEntryTransactionTest, CommitPreparedPackingTest) {
    gMaxNumberOfTransactionOperationsInSingleOplogEntry = std::numeric_limits<int>::max();
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    AutoGetCollection autoColl1(opCtx(), nss1, MODE_IX);

    std::vector<InsertStatement> inserts1;
    inserts1.emplace_back(0,
                          BSON("_id" << 0 << "data"
                                     << "x"));
    inserts1.emplace_back(1,
                          BSON("_id" << 1 << "data"
                                     << "y"));

    opObserver().onInserts(opCtx(),
                           *autoColl1,
                           inserts1.begin(),
                           inserts1.end(),
                           /*recordIds=*/{},
                           /*fromMigrate=*/std::vector<bool>(inserts1.size(), false),
                           /*defaultFromMigrate=*/false);

    auto reservedSlots = reserveOpTimesInSideTransaction(opCtx(), 2);
    auto prepareOpTime = reservedSlots.back();
    txnParticipant.transitionToPreparedforTest(opCtx(), prepareOpTime);
    prepareTransaction(reservedSlots, prepareOpTime);

    txnParticipant.stashTransactionResources(opCtx());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 1);

    // This should be the implicit prepare oplog entry.
    const auto insertEntry = assertGet(OplogEntry::parse(oplogEntryObjs[0]));
    ASSERT_TRUE(insertEntry.getOpType() == repl::OpTypeEnum::kCommand);
    ASSERT_TRUE(insertEntry.getCommandType() == OplogEntry::CommandType::kApplyOps);
    ASSERT_EQ(insertEntry.getObject()["prepare"].boolean(), true);

    // If we are only going to write a single prepare oplog entry, but we have reserved multiple
    // oplog slots, at T=1 and T=2, for example, then the 'prepare' oplog entry should be
    // written at T=2 i.e. the last reserved slot.  In this case, the 'startOpTime' of the
    // transaction should also be set to T=2, not T=1. We verify that below.
    const auto startOpTime = prepareOpTime;

    const auto prepareTimestamp = prepareOpTime.getTimestamp();

    // Reserve oplog entry for the commit oplog entry.
    OplogSlot commitSlot = reserveOpTimeInSideTransaction(opCtx());

    ASSERT_EQ(prepareOpTime, txnParticipant.getLastWriteOpTime());
    assertTxnRecord(txnNum(), prepareOpTime, DurableTxnStateEnum::kPrepared);
    assertTxnRecordStartOpTime(startOpTime);
    txnParticipant.unstashTransactionResources(opCtx(), "commitTransaction");

    // Mimic committing the transaction.
    shard_role_details::setWriteUnitOfWork(opCtx(), nullptr);
    shard_role_details::getLocker(opCtx())->unsetMaxLockTimeout();

    // commitTimestamp must be greater than the prepareTimestamp.
    auto commitTimestamp = Timestamp(prepareTimestamp.getSecs(), prepareTimestamp.getInc() + 1);

    opObserver().onPreparedTransactionCommit(
        opCtx(),
        commitSlot,
        commitTimestamp,
        txnParticipant.retrieveCompletedTransactionOperations(opCtx())
            ->getOperationsForOpObserver());

    oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    const auto commitOplogObj = oplogEntryObjs.back();
    checkSessionAndTransactionFields(commitOplogObj);
    auto commitEntry = assertGet(OplogEntry::parse(commitOplogObj));
    auto o = commitEntry.getObject();
    auto oExpected = BSON("commitTransaction" << 1 << "commitTimestamp" << commitTimestamp);
    ASSERT_BSONOBJ_EQ(oExpected, o);
    ASSERT_TRUE(commitEntry.getPrevWriteOpTimeInTransaction());
    ASSERT_EQ(*commitEntry.getPrevWriteOpTimeInTransaction(), insertEntry.getOpTime());

    assertTxnRecord(txnNum(), commitSlot, DurableTxnStateEnum::kCommitted);
    // startTimestamp should no longer be set once the transaction has been committed.
    assertTxnRecordStartOpTime(boost::none);
}

/**
 * Test fixture with sessions and an extra-large oplog for testing large transactions.
 */
class OpObserverLargeTransactionTest : public OpObserverTransactionTest {
private:
    repl::ReplSettings createReplSettings() override {
        repl::ReplSettings settings;
        // We need an oplog comfortably large enough to hold an oplog entry that exceeds the
        // BSON size limit.  Otherwise we will get the wrong error code when trying to write
        // one.
        settings.setOplogSizeBytes(BSONObjMaxInternalSize + 2 * 1024 * 1024);
        settings.setReplSetString("mySet/node1:12345");
        return settings;
    }
};

// Tests that a large transaction may be committed.  This test creates a transaction with two
// operations that together are just big enough to exceed the size limit, which should result in
// a two oplog entry transaction.
TEST_F(OpObserverLargeTransactionTest, LargeTransactionCreatesMultipleOplogEntries) {
    auto txnParticipant = TransactionParticipant::get(opCtx());
    txnParticipant.unstashTransactionResources(opCtx(), "insert");

    // This size is crafted such that two operations of this size are not too big to fit in a
    // single oplog entry, but two operations plus oplog overhead are too big to fit in a single
    // oplog entry.
    constexpr size_t kHalfTransactionSize = BSONObjMaxInternalSize / 2 - 175;
    std::unique_ptr<uint8_t[]> halfTransactionData(new uint8_t[kHalfTransactionSize]());
    auto operation1 = repl::DurableOplogEntry::makeInsertOperation(
        nss,
        uuid,
        BSON("_id" << 0 << "data"
                   << BSONBinData(halfTransactionData.get(), kHalfTransactionSize, BinDataGeneral)),
        BSON("_id" << 0));
    auto operation2 = repl::DurableOplogEntry::makeInsertOperation(
        nss,
        uuid,
        BSON("_id" << 0 << "data"
                   << BSONBinData(halfTransactionData.get(), kHalfTransactionSize, BinDataGeneral)),
        BSON("_id" << 0));
    txnParticipant.addTransactionOperation(opCtx(), operation1);
    txnParticipant.addTransactionOperation(opCtx(), operation2);
    auto txnOps = txnParticipant.retrieveCompletedTransactionOperations(opCtx());
    ASSERT_EQUALS(txnOps->getNumberOfPrePostImagesToWrite(), 0);
    commitUnpreparedTransaction<OpObserverImpl>(opCtx(), opObserver());
    auto oplogEntryObjs = getNOplogEntries(opCtx(), 2);
    std::vector<OplogEntry> oplogEntries;
    mongo::repl::OpTime expectedPrevWriteOpTime;
    for (const auto& oplogEntryObj : oplogEntryObjs) {
        checkSessionAndTransactionFields(oplogEntryObj);
        oplogEntries.push_back(assertGet(OplogEntry::parse(oplogEntryObj)));
        const auto& oplogEntry = oplogEntries.back();
        ASSERT(!oplogEntry.shouldPrepare());
        ASSERT_TRUE(oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_EQ(expectedPrevWriteOpTime, *oplogEntry.getPrevWriteOpTimeInTransaction());
        ASSERT_LT(expectedPrevWriteOpTime.getTimestamp(), oplogEntry.getTimestamp());
        expectedPrevWriteOpTime = repl::OpTime{oplogEntry.getTimestamp(), *oplogEntry.getTerm()};
    }

    auto oExpected = BSON("applyOps" << BSON_ARRAY(operation1.toBSON()) << "partialTxn" << true);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[0].getObject());

    oExpected = BSON("applyOps" << BSON_ARRAY(operation2.toBSON()) << "count" << 2);
    ASSERT_BSONOBJ_EQ(oExpected, oplogEntries[1].getObject());
}

TEST_F(OpObserverTest, OnRollbackInvalidatesDefaultRWConcernCache) {
    auto& rwcDefaults = ReadWriteConcernDefaults::get(getServiceContext());
    auto opCtx = getClient()->makeOperationContext();

    // Put initial defaults in the cache.
    {
        RWConcernDefault origDefaults;
        origDefaults.setUpdateOpTime(Timestamp(10, 20));
        origDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(1234));
        _lookupMock.setLookupCallReturnValue(std::move(origDefaults));
    }
    auto origCachedDefaults = rwcDefaults.getDefault(opCtx.get());
    ASSERT_EQ(Timestamp(10, 20), *origCachedDefaults.getUpdateOpTime());
    ASSERT_EQ(Date_t::fromMillisSinceEpoch(1234), *origCachedDefaults.getUpdateWallClockTime());

    // Change the mock's defaults, but don't invalidate the cache yet. The cache should still
    // return the original defaults.
    {
        RWConcernDefault newDefaults;
        newDefaults.setUpdateOpTime(Timestamp(50, 20));
        newDefaults.setUpdateWallClockTime(Date_t::fromMillisSinceEpoch(5678));
        _lookupMock.setLookupCallReturnValue(std::move(newDefaults));

        auto cachedDefaults = rwcDefaults.getDefault(opCtx.get());
        ASSERT_EQ(Timestamp(10, 20), *cachedDefaults.getUpdateOpTime());
        ASSERT_EQ(Date_t::fromMillisSinceEpoch(1234), *cachedDefaults.getUpdateWallClockTime());
    }

    // Rollback to a timestamp should invalidate the cache and getting the defaults should now
    // return the latest value.
    {
        OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
        OpObserver::RollbackObserverInfo rbInfo;
        opObserver.onReplicationRollback(opCtx.get(), rbInfo);
    }
    auto newCachedDefaults = rwcDefaults.getDefault(opCtx.get());
    ASSERT_EQ(Timestamp(50, 20), *newCachedDefaults.getUpdateOpTime());
    ASSERT_EQ(Date_t::fromMillisSinceEpoch(5678), *newCachedDefaults.getUpdateWallClockTime());
}

TEST_F(OpObserverTest, MagicRestoreNoOplog) {
    // Same as StartIndexBuildExpectedOplogEntry but with magicRestore = true.

    OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
    auto opCtx = cc().makeOperationContext();
    auto uuid = UUID::gen();
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(boost::none, "test.coll");
    UUID indexBuildUUID = UUID::gen();

    BSONObj specX = BSON("key" << BSON("x" << 1) << "name"
                               << "x_1"
                               << "v" << 2);
    BSONObj specA = BSON("key" << BSON("a" << 1) << "name"
                               << "a_1"
                               << "v" << 2);
    std::vector<BSONObj> specs = {specX, specA};

    storageGlobalParams.magicRestore = true;

    // Should not write to the oplog with magicRestore = true;
    {
        AutoGetDb autoDb(opCtx.get(), nss.dbName(), MODE_X);
        WriteUnitOfWork wunit(opCtx.get());
        opObserver.onStartIndexBuild(
            opCtx.get(), nss, uuid, indexBuildUUID, specs, false /*fromMigrate*/);
        wunit.commit();
    }

    getNOplogEntries(opCtx.get(), 0);  // This asserts that there are 0 entries.

    storageGlobalParams.magicRestore = false;
}

class OpObserverServerlessTest : public OpObserverTest {
private:
    // Need to set serverless.
    repl::ReplSettings createReplSettings() override {
        return repl::createServerlessReplSettings();
    }
};

TEST_F(OpObserverServerlessTest, OnInsertChecksIfTenantMigrationIsBlockingWrites) {
    auto opCtx = cc().makeOperationContext();

    // Add a tenant migration access blocker on donor for blocking writes.
    auto donorMtab = std::make_shared<TenantMigrationDonorAccessBlocker>(getServiceContext(), uuid);
    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).add(kTenantId, donorMtab);
    donorMtab->startBlockingWrites();

    std::vector<InsertStatement> insert;
    insert.emplace_back(BSON("_id" << 0 << "data"
                                   << "x"));

    {
        AutoGetCollection autoColl(opCtx.get(), kNssUnderTenantId, MODE_IX);
        OpObserverImpl opObserver(std::make_unique<OperationLoggerImpl>());
        ASSERT_THROWS_CODE(
            opObserver.onInserts(opCtx.get(),
                                 *autoColl,
                                 insert.begin(),
                                 insert.end(),
                                 /*recordIds=*/{},
                                 /*fromMigrate=*/std::vector<bool>(insert.size(), false),
                                 /*defaultFromMigrate=*/false),
            DBException,
            ErrorCodes::TenantMigrationConflict);
    }

    TenantMigrationAccessBlockerRegistry::get(getServiceContext()).shutDown();
}

}  // namespace
}  // namespace mongo
