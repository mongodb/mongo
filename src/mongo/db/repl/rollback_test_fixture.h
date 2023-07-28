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

#pragma once

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/read_write_concern_defaults_cache_lookup_mock.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/replication_recovery.h"
#include "mongo/db/repl/rollback_source.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/tenant_id.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace repl {

class RollbackTest : public ServiceContextMongoDTest {
public:
    explicit RollbackTest(Options options = {}) : ServiceContextMongoDTest(std::move(options)) {}

    /**
     * Initializes the service context and task executor.
     */
    void setUp() override;

    /**
     * Creates a collection with the given namespace and options.
     */
    static Collection* _createCollection(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const CollectionOptions& options);
    static Collection* _createCollection(OperationContext* opCtx,
                                         const std::string& nss,
                                         const CollectionOptions& options);

    /**
     * Inserts a single document into the collection namespace 'nss'.
     */
    void _insertDocument(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc);

    /**
     * Inserts a document into the oplog.
     */
    Status _insertOplogEntry(const BSONObj& doc);

    /**
     * Creates an oplog entry with a recordId for a CRUD operation (insert, update, delete). Also
     * works for creating a no-op entry.
     */
    static std::pair<BSONObj, RecordId> makeCRUDOp(OpTypeEnum opType,
                                                   Timestamp ts,
                                                   UUID uuid,
                                                   StringData nss,
                                                   BSONObj o,
                                                   boost::optional<BSONObj> o2,
                                                   int recordId);

    /**
     * Creates an oplog entry with a recordId for a command operation.
     */
    static std::pair<BSONObj, RecordId> makeCommandOp(Timestamp ts,
                                                      const boost::optional<UUID>& uuid,
                                                      StringData nss,
                                                      BSONObj cmdObj,
                                                      int recordId,
                                                      boost::optional<BSONObj> o2 = boost::none,
                                                      boost::optional<TenantId> tid = boost::none);

    /**
     * Creates an oplog entry with a recordId for a command operation. The oplog entry will not have
     * a "ts" or "wall" field. This is used for creating inner ops for applyOps entries.
     */
    static std::pair<BSONObj, RecordId> makeCommandOpForApplyOps(
        boost::optional<UUID> uuid,
        StringData nss,
        BSONObj cmdObj,
        int recordId,
        boost::optional<BSONObj> o2 = boost::none);

protected:
    // OperationContext provided to test cases for storage layer operations.
    ServiceContext::UniqueOperationContext _opCtx;

    // ReplicationCoordinator mock implementation for rollback tests.
    // Owned by service context.
    class ReplicationCoordinatorRollbackMock;
    ReplicationCoordinatorRollbackMock* _coordinator = nullptr;

    class StorageInterfaceRollback;
    StorageInterfaceRollback* _storageInterface = nullptr;
    ReplicationRecovery* _recovery;

    // ReplicationProcess used to access consistency markers.
    std::unique_ptr<ReplicationProcess> _replicationProcess;

    // DropPendingCollectionReaper used to clean up and roll back dropped collections.
    DropPendingCollectionReaper* _dropPendingCollectionReaper = nullptr;

    ReadWriteConcernDefaultsLookupMock _lookupMock;

    // Increase rollback log component verbosity for unit tests.
    unittest::MinimumLoggedSeverityGuard severityGuard{logv2::LogComponent::kReplicationRollback,
                                                       logv2::LogSeverity::Debug(2)};
};

class RollbackTest::StorageInterfaceRollback : public StorageInterfaceImpl {
public:
    void setStableTimestamp(ServiceContext* serviceCtx,
                            Timestamp snapshotName,
                            bool force = false) override {
        stdx::lock_guard<Latch> lock(_mutex);
        _stableTimestamp = snapshotName;
    }

    /**
     * If '_recoverToTimestampStatus' is non-empty, fasserts. If '_recoverToTimestampStatus' is
     * empty, updates '_currTimestamp' to be equal to '_stableTimestamp' and returns the new value
     * of '_currTimestamp'.
     */
    Timestamp recoverToStableTimestamp(OperationContext* opCtx) override {
        stdx::lock_guard<Latch> lock(_mutex);
        if (_recoverToTimestampStatus) {
            fassert(4584700, _recoverToTimestampStatus.get());
        }

        _currTimestamp = _stableTimestamp;
        return _currTimestamp;
    }

    bool supportsRecoverToStableTimestamp(ServiceContext* serviceCtx) const override {
        return true;
    }

    bool supportsRecoveryTimestamp(ServiceContext* serviceCtx) const override {
        return true;
    }

    boost::optional<Timestamp> getLastStableRecoveryTimestamp(
        ServiceContext* serviceCtx) const override {
        return _stableTimestamp;
    }

    void setRecoverToTimestampStatus(Status status) {
        stdx::lock_guard<Latch> lock(_mutex);
        _recoverToTimestampStatus = status;
    }

    void setCurrentTimestamp(Timestamp ts) {
        stdx::lock_guard<Latch> lock(_mutex);
        _currTimestamp = ts;
    }

    Timestamp getCurrentTimestamp() {
        stdx::lock_guard<Latch> lock(_mutex);
        return _currTimestamp;
    }

    /**
     * This function always expects to receive the UUID of the collection.
     */
    Status setCollectionCount(OperationContext* opCtx,
                              const NamespaceStringOrUUID& nsOrUUID,
                              long long newCount) {
        stdx::lock_guard<Latch> lock(_mutex);
        if (_setCollectionCountStatus && _setCollectionCountStatusUUID &&
            nsOrUUID.uuid() == _setCollectionCountStatusUUID) {
            return *_setCollectionCountStatus;
        }
        _newCounts[nsOrUUID.uuid()] = newCount;
        return Status::OK();
    }

    void setSetCollectionCountStatus(UUID uuid, Status status) {
        stdx::lock_guard<Latch> lock(_mutex);
        _setCollectionCountStatus = status;
        _setCollectionCountStatusUUID = uuid;
    }

    long long getFinalCollectionCount(const UUID& uuid) {
        stdx::lock_guard<Latch> lock(_mutex);
        return _newCounts[uuid];
    }

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("StorageInterfaceRollback::_mutex");

    Timestamp _stableTimestamp;

    // Used to mock the behavior of 'recoverToStableTimestamp'. Upon calling
    // 'recoverToStableTimestamp', the 'currTimestamp' should be set to the current
    // '_stableTimestamp' value. Can be viewed as mock version of replication's 'lastApplied'
    // optime.
    Timestamp _currTimestamp;

    // A Status value which, if set, will be returned by the 'recoverToStableTimestamp' function, in
    // order to simulate the error case for that function. Defaults to boost::none.
    boost::optional<Status> _recoverToTimestampStatus = boost::none;

    stdx::unordered_map<UUID, long long, UUID::Hash> _newCounts;

    boost::optional<Status> _setCollectionCountStatus = boost::none;
    boost::optional<UUID> _setCollectionCountStatusUUID = boost::none;
};

/**
 * ReplicationCoordinator mock implementation for rollback tests.
 */
class RollbackTest::ReplicationCoordinatorRollbackMock : public ReplicationCoordinatorMock {
public:
    ReplicationCoordinatorRollbackMock(ServiceContext* service);

    /**
     * Returns IllegalOperation (does not forward call to
     * ReplicationCoordinatorMock::setFollowerMode())
     * if new state requested is '_failSetFollowerModeOnThisMemberState'.
     * Otherwise, calls ReplicationCoordinatorMock::setFollowerMode().
     */
    Status setFollowerMode(const MemberState& newState) override;

    Status setFollowerModeRollback(OperationContext* opCtx) override;

    /**
     * Set this to make transitioning to the given follower mode fail with the given error code.
     */
    void failSettingFollowerMode(const MemberState& transitionToFail,
                                 ErrorCodes::Error codeToFailWith);

private:
    // Override this to make setFollowerMode() fail when called with this state.
    MemberState _failSetFollowerModeOnThisMemberState = MemberState::RS_UNKNOWN;

    ErrorCodes::Error _failSetFollowerModeWithThisCode = ErrorCodes::InternalError;
};

class RollbackSourceMock : public RollbackSource {
public:
    RollbackSourceMock(std::unique_ptr<OplogInterface> oplog);
    int getRollbackId() const override;
    const OplogInterface& getOplog() const override;
    const HostAndPort& getSource() const override;
    BSONObj getLastOperation() const override;
    BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const override;

    std::pair<BSONObj, NamespaceString> findOneByUUID(const DatabaseName& db,
                                                      UUID uuid,
                                                      const BSONObj& filter) const override;

    StatusWith<BSONObj> getCollectionInfoByUUID(const DatabaseName& dbName,
                                                const UUID& uuid) const override;
    StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const override;

private:
    std::unique_ptr<OplogInterface> _oplog;
    HostAndPort _source;
};

}  // namespace repl
}  // namespace mongo
