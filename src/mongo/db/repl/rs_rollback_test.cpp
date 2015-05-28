/**
 *    Copyright 2015 MongoDB Inc.
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

#include <list>
#include <memory>
#include <utility>

#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/client.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/minvalid.h"
#include "mongo/db/repl/operation_context_repl_mock.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_interface.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/rs_rollback.h"
#include "mongo/db/repl/rollback_source.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage_options.h"
#include "mongo/unittest/unittest.h"
#include "mongo/unittest/temp_dir.h"

namespace {

    using namespace mongo;
    using namespace mongo::repl;

    const Milliseconds globalWriteLockTimeoutMs(10);
    const OplogInterfaceMock::Operations kEmptyMockOperations;

    class OperationContextRollbackMock : public OperationContextReplMock {
    public:
        Client* getClient() const override;
    };

    Client* OperationContextRollbackMock::getClient() const {
        Client::initThreadIfNotAlready();
        return &cc();
    }

 ReplSettings createReplSettings() {
        ReplSettings settings;
        settings.oplogSize = 5 * 1024 * 1024;
        settings.replSet = "mySet/node1:12345";
        return settings;
    }

    class ReplicationCoordinatorRollbackMock : public ReplicationCoordinatorMock {
    public:
        ReplicationCoordinatorRollbackMock();
        void resetLastOpTimeFromOplog(OperationContext* txn) override;
    };

    ReplicationCoordinatorRollbackMock::ReplicationCoordinatorRollbackMock()
        : ReplicationCoordinatorMock(createReplSettings()) { }

    void ReplicationCoordinatorRollbackMock::resetLastOpTimeFromOplog(OperationContext* txn) { }

    class RollbackSourceMock : public RollbackSource {
    public:
        RollbackSourceMock(std::unique_ptr<OplogInterface> oplog);
        int getRollbackId() const override;
        const OplogInterface& getOplog() const override;
        BSONObj getLastOperation() const override;
        BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const override;
        void copyCollectionFromRemote(OperationContext* txn,
                                      const NamespaceString& nss) const override;
        StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const override;
    private:
        std::unique_ptr<OplogInterface> _oplog;
    };

    RollbackSourceMock::RollbackSourceMock(std::unique_ptr<OplogInterface> oplog)
        : _oplog(std::move(oplog)) { }

    const OplogInterface& RollbackSourceMock::getOplog() const {
        return *_oplog;
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

    void RollbackSourceMock::copyCollectionFromRemote(OperationContext* txn,
                                                      const NamespaceString& nss) const { }

    StatusWith<BSONObj> RollbackSourceMock::getCollectionInfo(const NamespaceString& nss) const {
        return BSON("name" << nss.ns() << "options" << BSONObj());
    }

    class RSRollbackTest : public unittest::Test {
    public:
        RSRollbackTest();

    protected:

        std::unique_ptr<OperationContext> _txn;
        std::unique_ptr<ReplicationCoordinator> _coordinator;

    private:
        void setUp() override;
        void tearDown() override;

        ReplicationCoordinator* _prevCoordinator;
    };

    RSRollbackTest::RSRollbackTest() : _prevCoordinator(nullptr) { }

    void RSRollbackTest::setUp() {
        ServiceContext* serviceContext = getGlobalServiceContext();
        if (!serviceContext->getGlobalStorageEngine()) {
            // When using the 'devnull' storage engine, it is fine for the temporary directory to
            // go away after the global storage engine is initialized.
            unittest::TempDir tempDir("rs_rollback_test");
            mongo::storageGlobalParams.dbpath = tempDir.path();
            mongo::storageGlobalParams.dbpath = tempDir.path();
            mongo::storageGlobalParams.engine = "inMemoryExperiment";
            mongo::storageGlobalParams.engineSetByUser = true;
            serviceContext->initializeGlobalStorageEngine();
        }

        Client::initThreadIfNotAlready();
        _txn.reset(new OperationContextRollbackMock());
        _coordinator.reset(new ReplicationCoordinatorRollbackMock());

        _prevCoordinator = getGlobalReplicationCoordinator();
        setGlobalReplicationCoordinator(_coordinator.get());

        setOplogCollectionName();
    }

    void RSRollbackTest::tearDown() {
        {
            Lock::GlobalWrite globalLock(_txn->lockState());
            BSONObjBuilder unused;
            invariant(mongo::dbHolder().closeAll(_txn.get(), unused, false));
        }
        setGlobalReplicationCoordinator(_prevCoordinator);
        _coordinator.reset();
        _txn.reset();
    }

    void noSleep(Seconds seconds) {}

    TEST_F(RSRollbackTest, InconsistentMinValid) {
        repl::setMinValid(_txn.get(), OpTime(Timestamp(Seconds(1), 0), 0));
        auto status =
            syncRollback(
                _txn.get(),
                OpTime(),
                OplogInterfaceMock(kEmptyMockOperations),
                RollbackSourceMock(std::unique_ptr<OplogInterface>(
                    new OplogInterfaceMock(kEmptyMockOperations))),
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs);
        ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
        ASSERT_EQUALS(18750, status.location());
    }

    TEST_F(RSRollbackTest, CannotObtainGlobalWriteLock) {
        OperationContextReplMock txn2;
        Lock::DBLock dbLock(txn2.lockState(), "test", MODE_X);
        ASSERT_FALSE(_txn->lockState()->isLocked());
        ASSERT_EQUALS(
            ErrorCodes::LockTimeout,
            syncRollback(
                _txn.get(),
                OpTime(),
                OplogInterfaceMock(kEmptyMockOperations),
                RollbackSourceMock(std::unique_ptr<OplogInterface>(
                    new OplogInterfaceMock(kEmptyMockOperations))),
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs).code());
    }

    TEST_F(RSRollbackTest, SetFollowerModeFailed) {
        class ReplicationCoordinatorSetFollowerModeMock : public ReplicationCoordinatorMock {
        public:
            ReplicationCoordinatorSetFollowerModeMock()
                : ReplicationCoordinatorMock(createReplSettings()) { }
            MemberState getMemberState() const override { return MemberState::RS_DOWN; }
            bool setFollowerMode(const MemberState& newState) override { return false; }
        };
        _coordinator.reset(new ReplicationCoordinatorSetFollowerModeMock());
        setGlobalReplicationCoordinator(_coordinator.get());

        ASSERT_EQUALS(
            ErrorCodes::OperationFailed,
            syncRollback(
                _txn.get(),
                OpTime(),
                OplogInterfaceMock(kEmptyMockOperations),
                RollbackSourceMock(std::unique_ptr<OplogInterface>(
                    new OplogInterfaceMock(kEmptyMockOperations))),
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs).code());
    }

    TEST_F(RSRollbackTest, OplogStartMissing) {
        OpTime ts(Timestamp(Seconds(1), 0), 0);
        auto operation =
            std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());
        ASSERT_EQUALS(
            ErrorCodes::OplogStartMissing,
            syncRollback(
                _txn.get(),
                OpTime(),
                OplogInterfaceMock(kEmptyMockOperations),
                RollbackSourceMock(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                    operation,
                }))),
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs).code());
    }

    TEST_F(RSRollbackTest, NoRemoteOpLog) {
        OpTime ts(Timestamp(Seconds(1), 0), 0);
        auto operation =
            std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());
        auto status =
            syncRollback(
                _txn.get(),
                ts,
                OplogInterfaceMock({operation}),
                RollbackSourceMock(std::unique_ptr<OplogInterface>(
                    new OplogInterfaceMock(kEmptyMockOperations))),
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs);
        ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
        ASSERT_EQUALS(18752, status.location());
    }

    TEST_F(RSRollbackTest, RemoteGetRollbackIdThrows) {
        OpTime ts(Timestamp(Seconds(1), 0), 0);
        auto operation =
            std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId());
        class RollbackSourceLocal : public RollbackSourceMock {
        public:
            RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
                : RollbackSourceMock(std::move(oplog)) { }
            int getRollbackId() const override {
                uassert(ErrorCodes::UnknownError, "getRollbackId() failed", false);
            }
        };
        ASSERT_THROWS_CODE(
            syncRollback(
                _txn.get(),
                ts,
                OplogInterfaceMock({operation}),
                RollbackSourceLocal(std::unique_ptr<OplogInterface>(
                    new OplogInterfaceMock(kEmptyMockOperations))),
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs),
            UserException,
            ErrorCodes::UnknownError);
    }

    TEST_F(RSRollbackTest, BothOplogsAtCommonPoint) {
        createOplog(_txn.get());
        OpTime ts(Timestamp(Seconds(1), 0), 1);
        auto operation =
            std::make_pair(BSON("ts" << ts.getTimestamp() << "h" << ts.getTerm()), RecordId(1));
        ASSERT_OK(
            syncRollback(
                _txn.get(),
                ts,
                OplogInterfaceMock({operation}),
                RollbackSourceMock(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                    operation,
                }))),
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs));
    }

    TEST_F(RSRollbackTest, FetchDeletedDocumentFromSource) {
        createOplog(_txn.get());
        auto commonOperation =
            std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
        auto deleteOperation =
            std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) <<
                                "h" << 1LL <<
                                "op" << "d" <<
                                "ns" << "test.t" <<
                                "o" << BSON("_id" << 0)),
                           RecordId(2));
        class RollbackSourceLocal : public RollbackSourceMock {
        public:
            RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
                : RollbackSourceMock(std::move(oplog)),
                  called(false) { }
            BSONObj findOne(const NamespaceString& nss, const BSONObj& filter) const {
                called = true;
                return BSONObj();
            }
            mutable bool called;
        };
        RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
            commonOperation,
        })));
        OpTime opTime(deleteOperation.first["ts"].timestamp(),
                      deleteOperation.first["h"].Long());
        ASSERT_OK(
            syncRollback(
                _txn.get(),
                opTime,
                OplogInterfaceMock({deleteOperation, commonOperation}),
                rollbackSource,
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs));
        ASSERT_TRUE(rollbackSource.called);
    }

    TEST_F(RSRollbackTest, RollbackUnknownCommand) {
        createOplog(_txn.get());
        auto commonOperation =
            std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
        auto unknownCommandOperation =
            std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) <<
                                "h" << 1LL <<
                                "op" << "c" <<
                                "ns" << "test.t" <<
                                "o" << BSON("unknown_command" << "t")),
                           RecordId(2));
        {
            Lock::DBLock dbLock(_txn->lockState(), "test", MODE_X);
            mongo::WriteUnitOfWork wuow(_txn.get());
            auto db = dbHolder().openDb(_txn.get(), "test");
            ASSERT_TRUE(db);
            ASSERT_TRUE(db->getOrCreateCollection(_txn.get(), "test.t"));
            wuow.commit();
        }
        OpTime opTime(unknownCommandOperation.first["ts"].timestamp(),
                      unknownCommandOperation.first["h"].Long());
        auto status =
            syncRollback(
                _txn.get(),
                opTime,
                OplogInterfaceMock({unknownCommandOperation, commonOperation}),
                RollbackSourceMock(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
                    commonOperation,
                }))),
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs);
        ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
        ASSERT_EQUALS(18751, status.location());
    }

// Re-enable after fixing recovery unit noop registerChange behavior
#if 0
    TEST_F(RSRollbackTest, RollbackDropCollectionCommand) {
        createOplog(_txn.get());
        auto commonOperation =
            std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
        auto dropCollectionOperation =
            std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) <<
                                "h" << 1LL <<
                                "op" << "c" <<
                                "ns" << "test.t" <<
                                "o" << BSON("drop" << "t")),
                           RecordId(2));
        class RollbackSourceLocal : public RollbackSourceMock {
        public:
            RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
                : RollbackSourceMock(std::move(oplog)),
                  called(false) { }
            void copyCollectionFromRemote(OperationContext* txn,
                                          const NamespaceString& nss) const override {
                called = true;
            }
            mutable bool called;
        };
        RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
            commonOperation,
        })));
        {
            Lock::DBLock dbLock(_txn->lockState(), "test", MODE_X);
            mongo::WriteUnitOfWork wuow(_txn.get());
            auto db = dbHolder().openDb(_txn.get(), "test");
            ASSERT_TRUE(db);
            ASSERT_TRUE(db->getOrCreateCollection(_txn.get(), "test.t"));
            wuow.commit();
        }
        OpTime opTime(dropCollectionOperation.first["ts"].timestamp(),
                      dropCollectionOperation.first["h"].Long());
        ASSERT_OK(
            syncRollback(
                _txn.get(),
                opTime,
                OplogInterfaceMock({dropCollectionOperation, commonOperation}),
                rollbackSource,
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs));
        ASSERT_TRUE(rollbackSource.called);
    }
#endif // 0

    TEST_F(RSRollbackTest, RollbackCollectionModificationCommand) {
        createOplog(_txn.get());
        auto commonOperation =
            std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
        auto collectionModificationOperation =
            std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) <<
                                "h" << 1LL <<
                                "op" << "c" <<
                                "ns" << "test.t" <<
                                "o" << BSON("collMod" << "t" << "noPadding" << false)),
                           RecordId(2));
        class RollbackSourceLocal : public RollbackSourceMock {
        public:
            RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
                : RollbackSourceMock(std::move(oplog)),
                  called(false) { }
            StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const {
                called = true;
                return RollbackSourceMock::getCollectionInfo(nss);
            }
            mutable bool called;
        };
        RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
            commonOperation,
        })));
        {
            Lock::DBLock dbLock(_txn->lockState(), "test", MODE_X);
            mongo::WriteUnitOfWork wuow(_txn.get());
            auto db = dbHolder().openDb(_txn.get(), "test");
            ASSERT_TRUE(db);
            ASSERT_TRUE(db->getOrCreateCollection(_txn.get(), "test.t"));
            wuow.commit();
        }
        OpTime opTime(collectionModificationOperation.first["ts"].timestamp(),
                      collectionModificationOperation.first["h"].Long());
        ASSERT_OK(
            syncRollback(
                _txn.get(),
                opTime,
                OplogInterfaceMock({collectionModificationOperation, commonOperation}),
                rollbackSource,
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs));
        ASSERT_TRUE(rollbackSource.called);
    }

    TEST_F(RSRollbackTest, RollbackCollectionModificationCommandInvalidCollectionOptions) {
        createOplog(_txn.get());
        auto commonOperation =
            std::make_pair(BSON("ts" << Timestamp(Seconds(1), 0) << "h" << 1LL), RecordId(1));
        auto collectionModificationOperation =
            std::make_pair(BSON("ts" << Timestamp(Seconds(2), 0) <<
                                "h" << 1LL <<
                                "op" << "c" <<
                                "ns" << "test.t" <<
                                "o" << BSON("collMod" << "t" << "noPadding" << false)),
                           RecordId(2));
        class RollbackSourceLocal : public RollbackSourceMock {
        public:
            RollbackSourceLocal(std::unique_ptr<OplogInterface> oplog)
                : RollbackSourceMock(std::move(oplog)) { }
            StatusWith<BSONObj> getCollectionInfo(const NamespaceString& nss) const {
                return BSON("name" << nss.ns() << "options" << 12345);
            }
        };
        RollbackSourceLocal rollbackSource(std::unique_ptr<OplogInterface>(new OplogInterfaceMock({
            commonOperation,
        })));
        {
            Lock::DBLock dbLock(_txn->lockState(), "test", MODE_X);
            mongo::WriteUnitOfWork wuow(_txn.get());
            auto db = dbHolder().openDb(_txn.get(), "test");
            ASSERT_TRUE(db);
            ASSERT_TRUE(db->getOrCreateCollection(_txn.get(), "test.t"));
            wuow.commit();
        }
        OpTime opTime(collectionModificationOperation.first["ts"].timestamp(),
                      collectionModificationOperation.first["h"].Long());
        auto status =
            syncRollback(
                _txn.get(),
                opTime,
                OplogInterfaceMock({collectionModificationOperation, commonOperation}),
                rollbackSource,
                _coordinator.get(),
                noSleep,
                globalWriteLockTimeoutMs);
        ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status.code());
        ASSERT_EQUALS(18753, status.location());
    }

} // namespace
