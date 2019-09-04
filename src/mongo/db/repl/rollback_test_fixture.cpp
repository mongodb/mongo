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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rollback_test_fixture.h"

#include <memory>
#include <string>

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/op_observer_noop.h"
#include "mongo/db/op_observer_registry.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/logger.h"
#include "mongo/util/str.h"

namespace mongo {
namespace repl {

namespace {

/**
 * Creates ReplSettings for ReplicationCoordinatorRollbackMock.
 */
ReplSettings createReplSettings() {
    ReplSettings settings;
    settings.setOplogSizeBytes(5 * 1024 * 1024);
    settings.setReplSetString("mySet/node1:12345");
    return settings;
}

class RollbackTestOpObserver : public OpObserverNoop {
public:
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  OptionalCollectionUUID uuid,
                                  std::uint64_t numRecords,
                                  const CollectionDropType dropType) override {
        // If the oplog is not disabled for this namespace, then we need to reserve an op time for
        // the drop.
        if (!repl::ReplicationCoordinator::get(opCtx)->isOplogDisabledFor(opCtx, collectionName)) {
            OpObserver::Times::get(opCtx).reservedOpTimes.push_back(dropOpTime);
        }
        return {};
    }

    const repl::OpTime dropOpTime = {Timestamp(Seconds(100), 1U), 1LL};
};

}  // namespace

void RollbackTest::setUp() {
    _storageInterface = new StorageInterfaceRollback();
    auto serviceContext = getServiceContext();
    auto consistencyMarkers = std::make_unique<ReplicationConsistencyMarkersMock>();
    auto recovery =
        std::make_unique<ReplicationRecoveryImpl>(_storageInterface, consistencyMarkers.get());
    _replicationProcess = std::make_unique<ReplicationProcess>(
        _storageInterface, std::move(consistencyMarkers), std::move(recovery));
    _dropPendingCollectionReaper = new DropPendingCollectionReaper(_storageInterface);
    DropPendingCollectionReaper::set(
        serviceContext, std::unique_ptr<DropPendingCollectionReaper>(_dropPendingCollectionReaper));
    StorageInterface::set(serviceContext, std::unique_ptr<StorageInterface>(_storageInterface));
    _coordinator = new ReplicationCoordinatorRollbackMock(serviceContext);
    ReplicationCoordinator::set(serviceContext,
                                std::unique_ptr<ReplicationCoordinator>(_coordinator));
    setOplogCollectionName(serviceContext);

    _opCtx = makeOperationContext();
    _replicationProcess->getConsistencyMarkers()->clearAppliedThrough(_opCtx.get(), {});
    _replicationProcess->getConsistencyMarkers()->setMinValid(_opCtx.get(), OpTime{});
    _replicationProcess->initializeRollbackID(_opCtx.get()).transitional_ignore();

    // Increase rollback log component verbosity for unit tests.
    mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        logger::LogComponent::kReplicationRollback, logger::LogSeverity::Debug(2));

    auto observerRegistry = checked_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
    observerRegistry->addObserver(std::make_unique<RollbackTestOpObserver>());
}

RollbackTest::ReplicationCoordinatorRollbackMock::ReplicationCoordinatorRollbackMock(
    ServiceContext* service)
    : ReplicationCoordinatorMock(service, createReplSettings()) {}

void RollbackTest::ReplicationCoordinatorRollbackMock::failSettingFollowerMode(
    const MemberState& transitionToFail, ErrorCodes::Error codeToFailWith) {
    _failSetFollowerModeOnThisMemberState = transitionToFail;
    _failSetFollowerModeWithThisCode = codeToFailWith;
}

Status RollbackTest::ReplicationCoordinatorRollbackMock::setFollowerMode(
    const MemberState& newState) {
    if (newState == _failSetFollowerModeOnThisMemberState) {
        return Status(_failSetFollowerModeWithThisCode,
                      str::stream()
                          << "ReplicationCoordinatorRollbackMock set to fail on setting state to "
                          << _failSetFollowerModeOnThisMemberState.toString());
    }
    return ReplicationCoordinatorMock::setFollowerMode(newState);
}

Status RollbackTest::ReplicationCoordinatorRollbackMock::setFollowerModeStrict(
    OperationContext* opCtx, const MemberState& newState) {
    return setFollowerMode(newState);
}

std::pair<BSONObj, RecordId> RollbackTest::makeCRUDOp(OpTypeEnum opType,
                                                      Timestamp ts,
                                                      UUID uuid,
                                                      StringData nss,
                                                      BSONObj o,
                                                      boost::optional<BSONObj> o2,
                                                      int recordId) {
    invariant(opType != OpTypeEnum::kCommand);

    BSONObjBuilder bob;
    bob.append("ts", ts);
    bob.append("op", OpType_serializer(opType));
    uuid.appendToBuilder(&bob, "ui");
    bob.append("ns", nss);
    bob.append("o", o);
    if (o2) {
        bob.append("o2", *o2);
    }
    bob.append("wall", Date_t());

    return std::make_pair(bob.obj(), RecordId(recordId));
}


std::pair<BSONObj, RecordId> RollbackTest::makeCommandOp(Timestamp ts,
                                                         OptionalCollectionUUID uuid,
                                                         StringData nss,
                                                         BSONObj cmdObj,
                                                         int recordId,
                                                         boost::optional<BSONObj> o2) {

    BSONObjBuilder bob;
    bob.append("ts", ts);
    bob.append("op", "c");
    if (uuid) {  // Not all ops have UUID fields.
        uuid.get().appendToBuilder(&bob, "ui");
    }
    bob.append("ns", nss);
    bob.append("o", cmdObj);
    if (o2) {
        bob.append("o2", *o2);
    }
    bob.append("wall", Date_t());

    return std::make_pair(bob.obj(), RecordId(recordId));
}

Collection* RollbackTest::_createCollection(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const CollectionOptions& options) {
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
    mongo::WriteUnitOfWork wuow(opCtx);
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->openDb(opCtx, nss.db());
    ASSERT_TRUE(db);
    db->dropCollection(opCtx, nss).transitional_ignore();
    auto coll = db->createCollection(opCtx, nss, options);
    ASSERT_TRUE(coll);
    wuow.commit();
    return coll;
}

Collection* RollbackTest::_createCollection(OperationContext* opCtx,
                                            const std::string& nss,
                                            const CollectionOptions& options) {
    return _createCollection(opCtx, NamespaceString(nss), options);
}

Status RollbackTest::_insertOplogEntry(const BSONObj& doc) {
    TimestampedBSONObj obj;
    obj.obj = doc;
    return _storageInterface->insertDocument(
        _opCtx.get(), NamespaceString::kRsOplogNamespace, obj, 0);
}

RollbackSourceMock::RollbackSourceMock(std::unique_ptr<OplogInterface> oplog)
    : _oplog(std::move(oplog)) {}

const OplogInterface& RollbackSourceMock::getOplog() const {
    return *_oplog;
}

const HostAndPort& RollbackSourceMock::getSource() const {
    return _source;
}

int RollbackSourceMock::getRollbackId() const {
    return 0;
}

BSONObj RollbackSourceMock::getLastOperation() const {
    auto iter = _oplog->makeIterator();
    auto result = iter->next();
    ASSERT_OK(result.getStatus());
    return result.getValue().first;
}

BSONObj RollbackSourceMock::findOne(const NamespaceString& nss, const BSONObj& filter) const {
    return BSONObj();
}

std::pair<BSONObj, NamespaceString> RollbackSourceMock::findOneByUUID(const std::string& db,
                                                                      UUID uuid,
                                                                      const BSONObj& filter) const {
    return {BSONObj(), NamespaceString()};
}

void RollbackSourceMock::copyCollectionFromRemote(OperationContext* opCtx,
                                                  const NamespaceString& nss) const {}

StatusWith<BSONObj> RollbackSourceMock::getCollectionInfo(const NamespaceString& nss) const {
    return BSON("name" << nss.ns() << "options" << BSONObj());
}

StatusWith<BSONObj> RollbackSourceMock::getCollectionInfoByUUID(const std::string& db,
                                                                const UUID& uuid) const {
    return BSON("options" << BSONObj() << "info" << BSON("uuid" << uuid));
}

RollbackResyncsCollectionOptionsTest::RollbackSourceWithCollectionOptions::
    RollbackSourceWithCollectionOptions(std::unique_ptr<OplogInterface> oplog,
                                        BSONObj collOptionsObj)
    : RollbackSourceMock(std::move(oplog)), collOptionsObj(collOptionsObj) {}

StatusWith<BSONObj>
RollbackResyncsCollectionOptionsTest::RollbackSourceWithCollectionOptions::getCollectionInfoByUUID(
    const std::string& db, const UUID& uuid) const {
    return BSON("options" << collOptionsObj << "info" << BSON("uuid" << uuid));
}

void RollbackResyncsCollectionOptionsTest::resyncCollectionOptionsTest(
    CollectionOptions localCollOptions, BSONObj remoteCollOptionsObj) {
    resyncCollectionOptionsTest(localCollOptions,
                                remoteCollOptionsObj,
                                BSON("collMod"
                                     << "coll"
                                     << "validationLevel"
                                     << "strict"),
                                "coll");
}
void RollbackResyncsCollectionOptionsTest::resyncCollectionOptionsTest(
    CollectionOptions localCollOptions,
    BSONObj remoteCollOptionsObj,
    BSONObj collModCmd,
    std::string collName) {
    createOplog(_opCtx.get());

    auto dbName = "test";
    auto nss = NamespaceString(dbName, collName);

    auto coll = _createCollection(_opCtx.get(), nss.toString(), localCollOptions);

    auto commonOpUuid = unittest::assertGet(UUID::parse("f005ba11-cafe-bead-f00d-123456789abc"));
    auto commonOpBson = BSON("ts" << Timestamp(1, 1) << "t" << 1LL << "op"
                                  << "n"
                                  << "o" << BSONObj() << "ns"
                                  << "rollback_test.test"
                                  << "wall" << Date_t() << "ui" << commonOpUuid);

    auto commonOperation = std::make_pair(commonOpBson, RecordId(1));

    auto collectionModificationOperation =
        makeCommandOp(Timestamp(Seconds(2), 0), coll->uuid(), nss.toString(), collModCmd, 2);

    RollbackSourceWithCollectionOptions rollbackSource(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})),
        remoteCollOptionsObj);

    ASSERT_OK(syncRollback(_opCtx.get(),
                           OplogInterfaceMock({collectionModificationOperation, commonOperation}),
                           rollbackSource,
                           {},
                           _coordinator,
                           _replicationProcess.get()));

    // Make sure the collection options are correct.
    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), NamespaceString(nss.toString()));
    auto collAfterRollbackOptions =
        DurableCatalog::get(_opCtx.get())->getCollectionOptions(_opCtx.get(), nss);

    BSONObjBuilder expectedOptionsBob;
    if (localCollOptions.uuid) {
        localCollOptions.uuid.get().appendToBuilder(&expectedOptionsBob, "uuid");
    }
    expectedOptionsBob.appendElements(remoteCollOptionsObj);

    ASSERT_BSONOBJ_EQ(expectedOptionsBob.obj(), collAfterRollbackOptions.toBSON());
}
}  // namespace repl
}  // namespace mongo
