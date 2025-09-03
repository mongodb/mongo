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

#include "mongo/db/repl/rollback_test_fixture.h"

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/local_catalog/database.h"
#include "mongo/db/local_catalog/database_holder.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_noop.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/read_write_concern_defaults.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_consistency_markers_mock.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/session/session_catalog_mongod.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/transaction/session_catalog_mongod_transaction_interface_impl.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#include <cstdint>
#include <string>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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

StorageEngine::TimestampMonitor::TimestampListener mockTimestampListener(
    StorageEngine::TimestampMonitor::TimestampType::kOldest,
    [](OperationContext* opCtx, Timestamp timestamp) {});

class RollbackTestOpObserver : public OpObserverNoop {
public:
    repl::OpTime onDropCollection(OperationContext* opCtx,
                                  const NamespaceString& collectionName,
                                  const UUID& uuid,
                                  std::uint64_t numRecords,
                                  bool markFromMigrate,
                                  bool isViewlessTimeseries) override {
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
    ServiceContextMongoDTest::setUp();
    cc().setOperationUnkillable_ForTest();

    _storageInterface = new StorageInterfaceRollback();
    auto serviceContext = getServiceContext();
    serviceContext->getStorageEngine()->startTimestampMonitor({&mockTimestampListener});
    auto consistencyMarkers = std::make_unique<ReplicationConsistencyMarkersMock>();
    auto recovery =
        std::make_unique<ReplicationRecoveryImpl>(_storageInterface, consistencyMarkers.get());
    _replicationProcess = std::make_unique<ReplicationProcess>(
        _storageInterface, std::move(consistencyMarkers), std::move(recovery));
    StorageInterface::set(serviceContext, std::unique_ptr<StorageInterface>(_storageInterface));
    _coordinator = new ReplicationCoordinatorRollbackMock(serviceContext);
    ReplicationCoordinator::set(serviceContext,
                                std::unique_ptr<ReplicationCoordinator>(_coordinator));

    _opCtx = makeOperationContext();
    _replicationProcess->getConsistencyMarkers()->clearAppliedThrough(_opCtx.get());
    _replicationProcess->initializeRollbackID(_opCtx.get()).transitional_ignore();

    MongoDSessionCatalog::set(
        serviceContext,
        std::make_unique<MongoDSessionCatalog>(
            std::make_unique<MongoDSessionCatalogTransactionInterfaceImpl>()));

    auto observerRegistry = checked_cast<OpObserverRegistry*>(serviceContext->getOpObserver());
    observerRegistry->addObserver(std::make_unique<RollbackTestOpObserver>());

    ReadWriteConcernDefaults::create(getService(), _lookupMock.getFetchDefaultsFn());
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

Status RollbackTest::ReplicationCoordinatorRollbackMock::setFollowerModeRollback(
    OperationContext* opCtx) {
    return setFollowerMode(MemberState::RS_ROLLBACK);
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
                                                         const boost::optional<UUID>& uuid,
                                                         const NamespaceString& nss,
                                                         BSONObj cmdObj,
                                                         int recordId,
                                                         boost::optional<BSONObj> o2,
                                                         boost::optional<TenantId> tid) {

    BSONObjBuilder bob;
    bob.append("ts", ts);
    bob.append("op", "c");
    if (uuid) {  // Not all ops have UUID fields.
        uuid.value().appendToBuilder(&bob, "ui");
    }
    if (tid)
        tid->serializeToBSON("tid", &bob);
    bob.append("ns", NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault()));
    bob.append("o", cmdObj);
    if (o2) {
        bob.append("o2", *o2);
    }
    bob.append("wall", Date_t());

    return std::make_pair(bob.obj(), RecordId(recordId));
}

std::pair<BSONObj, RecordId> RollbackTest::makeCommandOpForApplyOps(boost::optional<UUID> uuid,
                                                                    StringData nss,
                                                                    BSONObj cmdObj,
                                                                    int recordId,
                                                                    boost::optional<BSONObj> o2) {
    BSONObjBuilder bob;
    bob.append("op", "c");
    if (uuid) {  // Not all ops have UUID fields.
        uuid.value().appendToBuilder(&bob, "ui");
    }
    bob.append("ns", nss);
    bob.append("o", cmdObj);
    if (o2) {
        bob.append("o2", *o2);
    }

    return std::make_pair(bob.obj(), RecordId(recordId));
}

Collection* RollbackTest::_createCollection(OperationContext* opCtx,
                                            const NamespaceString& nss,
                                            const CollectionOptions& options) {
    Lock::DBLock dbLock(opCtx, nss.dbName(), MODE_X);
    mongo::WriteUnitOfWork wuow(opCtx);
    auto databaseHolder = DatabaseHolder::get(opCtx);
    auto db = databaseHolder->openDb(opCtx, nss.dbName());
    ASSERT_TRUE(db);
    db->dropCollection(opCtx, nss).transitional_ignore();
    auto coll = db->createCollection(opCtx, nss, options);
    ASSERT_TRUE(coll);
    wuow.commit();
    return coll;
}

Status RollbackTest::_insertOplogEntry(const BSONObj& doc) {
    TimestampedBSONObj obj;
    obj.obj = doc;
    _storageInterface->oplogDiskLocRegister(_opCtx.get(), doc["ts"].timestamp(), true);
    return _storageInterface->insertDocument(
        _opCtx.get(), NamespaceString::kRsOplogNamespace, obj, 0);
}

}  // namespace repl
}  // namespace mongo
