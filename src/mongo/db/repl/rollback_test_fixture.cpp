/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rollback_test_fixture.h"

#include <string>

#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery_mock.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/repl/rs_rollback_no_uuid.h"
#include "mongo/db/session_catalog.h"
#include "mongo/logger/log_component.h"
#include "mongo/logger/logger.h"

#include "mongo/stdx/memory.h"
#include "mongo/util/mongoutils/str.h"

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

}  // namespace

void RollbackTest::setUp() {
    _serviceContextMongoDTest.setUp();
    auto serviceContext = _serviceContextMongoDTest.getServiceContext();
    _replicationProcess = stdx::make_unique<ReplicationProcess>(
        &_storageInterface,
        stdx::make_unique<ReplicationConsistencyMarkersMock>(),
        stdx::make_unique<ReplicationRecoveryMock>());
    _dropPendingCollectionReaper = new DropPendingCollectionReaper(&_storageInterface);
    DropPendingCollectionReaper::set(
        serviceContext, std::unique_ptr<DropPendingCollectionReaper>(_dropPendingCollectionReaper));
    _coordinator = new ReplicationCoordinatorRollbackMock(serviceContext);
    ReplicationCoordinator::set(serviceContext,
                                std::unique_ptr<ReplicationCoordinator>(_coordinator));
    setOplogCollectionName();

    SessionCatalog::create(serviceContext);

    _opCtx = cc().makeOperationContext();
    _replicationProcess->getConsistencyMarkers()->setAppliedThrough(_opCtx.get(), OpTime{});
    _replicationProcess->getConsistencyMarkers()->setMinValid(_opCtx.get(), OpTime{});
    _replicationProcess->initializeRollbackID(_opCtx.get()).transitional_ignore();

    // Increase rollback log component verbosity for unit tests.
    mongo::logger::globalLogDomain()->setMinimumLoggedSeverity(
        logger::LogComponent::kReplicationRollback, logger::LogSeverity::Debug(2));
}

void RollbackTest::tearDown() {
    _coordinator = nullptr;
    _opCtx.reset();

    SessionCatalog::reset_forTest(_serviceContextMongoDTest.getServiceContext());

    // We cannot unset the global replication coordinator because ServiceContextMongoD::tearDown()
    // calls dropAllDatabasesExceptLocal() which requires the replication coordinator to clear all
    // snapshots.
    _serviceContextMongoDTest.tearDown();

    // ServiceContextMongoD::tearDown() does not destroy service context so it is okay
    // to access the service context after tearDown().
    auto serviceContext = _serviceContextMongoDTest.getServiceContext();
    _replicationProcess.reset();
    ReplicationCoordinator::set(serviceContext, {});
}

RollbackTest::ReplicationCoordinatorRollbackMock::ReplicationCoordinatorRollbackMock(
    ServiceContext* service)
    : ReplicationCoordinatorMock(service, createReplSettings()) {}

void RollbackTest::ReplicationCoordinatorRollbackMock::resetLastOpTimesFromOplog(
    OperationContext* opCtx, ReplicationCoordinator::DataConsistency consistency) {}

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

std::pair<BSONObj, RecordId> RollbackTest::makeCommandOp(
    Timestamp ts, OptionalCollectionUUID uuid, StringData nss, BSONObj cmdObj, int recordId) {

    BSONObjBuilder bob;
    bob.append("ts", ts);
    bob.append("h", 1LL);
    bob.append("op", "c");
    if (uuid) {  // Not all ops have UUID fields.
        uuid.get().appendToBuilder(&bob, "ui");
    }
    bob.append("ns", nss);
    bob.append("o", cmdObj);

    return std::make_pair(bob.obj(), RecordId(recordId));
}

Collection* RollbackTest::_createCollection(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const CollectionOptions& options) {
    Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
    mongo::WriteUnitOfWork wuow(opCtx);
    auto db = dbHolder().openDb(opCtx, nss.db());
    ASSERT_TRUE(db);
    db->dropCollection(opCtx, nss.ns()).transitional_ignore();
    auto coll = db->createCollection(opCtx, nss.ns(), options);
    ASSERT_TRUE(coll);
    wuow.commit();
    return coll;
}

Collection* RollbackTest::_createCollection(OperationContext* opCtx,
                                            const std::string& nss,
                                            const CollectionOptions& options) {
    return _createCollection(opCtx, NamespaceString(nss), options);
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
RollbackResyncsCollectionOptionsTest::RollbackSourceWithCollectionOptions::getCollectionInfo(
    const NamespaceString& nss) const {
    calledNoUUID = true;
    return BSON("options" << collOptionsObj);
}

StatusWith<BSONObj>
RollbackResyncsCollectionOptionsTest::RollbackSourceWithCollectionOptions::getCollectionInfoByUUID(
    const std::string& db, const UUID& uuid) const {
    calledWithUUID = true;
    return BSON("options" << collOptionsObj << "info" << BSON("uuid" << uuid));
}

void RollbackResyncsCollectionOptionsTest::resyncCollectionOptionsTest(
    CollectionOptions localCollOptions, BSONObj remoteCollOptionsObj) {
    resyncCollectionOptionsTest(localCollOptions,
                                remoteCollOptionsObj,
                                BSON("collMod"
                                     << "coll"
                                     << "noPadding"
                                     << false),
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
    auto commonOperation =
        std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));

    auto collectionModificationOperation =
        makeCommandOp(Timestamp(Seconds(2), 0), coll->uuid(), nss.toString(), collModCmd, 2);

    RollbackSourceWithCollectionOptions rollbackSource(
        std::unique_ptr<OplogInterface>(new OplogInterfaceMock({commonOperation})),
        remoteCollOptionsObj);

    if (coll->uuid()) {
        ASSERT_OK(
            syncRollback(_opCtx.get(),
                         OplogInterfaceMock({collectionModificationOperation, commonOperation}),
                         rollbackSource,
                         {},
                         _coordinator,
                         _replicationProcess.get()));

        ASSERT_TRUE(rollbackSource.calledWithUUID);
        ASSERT_FALSE(rollbackSource.calledNoUUID);
    } else {
        ASSERT_OK(syncRollbackNoUUID(
            _opCtx.get(),
            OplogInterfaceMock({collectionModificationOperation, commonOperation}),
            rollbackSource,
            {},
            _coordinator,
            _replicationProcess.get()));

        ASSERT_TRUE(rollbackSource.calledNoUUID);
        ASSERT_FALSE(rollbackSource.calledWithUUID);
    }

    // Make sure the collection options are correct.
    AutoGetCollectionForReadCommand autoColl(_opCtx.get(), NamespaceString(nss.toString()));
    auto collAfterRollbackOptions =
        autoColl.getCollection()->getCatalogEntry()->getCollectionOptions(_opCtx.get());

    BSONObjBuilder expectedOptionsBob;
    if (localCollOptions.uuid) {
        localCollOptions.uuid.get().appendToBuilder(&expectedOptionsBob, "uuid");
    }
    expectedOptionsBob.appendElements(remoteCollOptionsObj);

    ASSERT_BSONOBJ_EQ(expectedOptionsBob.obj(), collAfterRollbackOptions.toBSON());
}
}  // namespace repl
}  // namespace mongo
