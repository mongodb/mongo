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

#pragma once

#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/rollback_source.h"
#include "mongo/db/repl/storage_interface_impl.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"

namespace mongo {
namespace repl {

/**
 * Test fixture for both 3.4 and 3.6 rollback unit tests.
 * The fixture makes available to tests:
 * - an "ephemeralForTest" storage engine for checking results of the rollback algorithm at the
 *   storage layer. The storage engine is initialized as part of the ServiceContextForMongoD test
 *   fixture.
 */
class RollbackTest : public unittest::Test {
public:
    RollbackTest() = default;

    /**
     * Initializes the service context and task executor.
     */
    void setUp() override;

    /**
     * Destroys the service context and task executor.
     *
     * Note on overriding tearDown() in tests:
     * This cancels outstanding tasks and remote command requests scheduled using the task
     * executor.
     */
    void tearDown() override;

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
     * Creates an oplog entry with a recordId for a command operation.
     */
    static std::pair<BSONObj, RecordId> makeCommandOp(
        Timestamp ts, OptionalCollectionUUID uuid, StringData nss, BSONObj cmdObj, int recordId);

protected:
    // Test fixture used to manage the service context and global storage engine.
    ServiceContextMongoDTest _serviceContextMongoDTest;

    // OperationContext provided to test cases for storage layer operations.
    ServiceContext::UniqueOperationContext _opCtx;

    // ReplicationCoordinator mock implementation for rollback tests.
    // Owned by service context.
    class ReplicationCoordinatorRollbackMock;
    ReplicationCoordinatorRollbackMock* _coordinator = nullptr;

    StorageInterfaceImpl _storageInterface;

    // ReplicationProcess used to access consistency markers.
    std::unique_ptr<ReplicationProcess> _replicationProcess;

    // DropPendingCollectionReaper used to clean up and roll back dropped collections.
    DropPendingCollectionReaper* _dropPendingCollectionReaper = nullptr;
};

/**
 * ReplicationCoordinator mock implementation for rollback tests.
 */
class RollbackTest::ReplicationCoordinatorRollbackMock : public ReplicationCoordinatorMock {
public:
    ReplicationCoordinatorRollbackMock(ServiceContext* service);

    /**
     * Base class implementation triggers an invariant. This function is overridden to be a no-op
     * for rollback tests.
     */
    void resetLastOpTimesFromOplog(OperationContext* opCtx,
                                   ReplicationCoordinator::DataConsistency consistency) override;

    /**
     * Returns IllegalOperation (does not forward call to
     * ReplicationCoordinatorMock::setFollowerMode())
     * if new state requested is '_failSetFollowerModeOnThisMemberState'.
     * Otherwise, calls ReplicationCoordinatorMock::setFollowerMode().
     */
    Status setFollowerMode(const MemberState& newState) override;

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

    std::pair<BSONObj, NamespaceString> findOneByUUID(const std::string& db,
                                                      UUID uuid,
                                                      const BSONObj& filter) const override;

    void copyCollectionFromRemote(OperationContext* opCtx,
                                  const NamespaceString& nss) const override;
    StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db,
                                                const UUID& uuid) const override;
    StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const override;

private:
    std::unique_ptr<OplogInterface> _oplog;
    HostAndPort _source;
};

/**
 * Test fixture to ensure that rollback re-syncs collection options from a sync source and updates
 * the local collection options correctly. A test operates on a single test collection, and is
 * parameterized on two arguments:
 *
 * 'localCollOptions': the collection options that the local test collection is initially created
 * with.
 *
 * 'remoteCollOptionsObj': the collection options object that the sync source will respond with to
 * the rollback node when it fetches collection metadata.
 *
 * If no command is provided, a collMod operation with a 'noPadding' argument is used to trigger a
 * collection metadata resync, since the rollback of collMod operations does not take into account
 * the actual command object. It simply re-syncs all the collection options.
 */
class RollbackResyncsCollectionOptionsTest : public RollbackTest {

    class RollbackSourceWithCollectionOptions : public RollbackSourceMock {
    public:
        RollbackSourceWithCollectionOptions(std::unique_ptr<OplogInterface> oplog,
                                            BSONObj collOptionsObj);

        StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const override;
        StatusWith<BSONObj> getCollectionInfoByUUID(const std::string& db,
                                                    const UUID& uuid) const override;

        mutable bool calledNoUUID = false;
        mutable bool calledWithUUID = false;
        BSONObj collOptionsObj;
    };

public:
    void resyncCollectionOptionsTest(CollectionOptions localCollOptions,
                                     BSONObj remoteCollOptionsObj,
                                     BSONObj collModCmd,
                                     std::string collName);
    void resyncCollectionOptionsTest(CollectionOptions localCollOptions,
                                     BSONObj remoteCollOptionsObj);
};

}  // namespace repl
}  // namespace mongo
