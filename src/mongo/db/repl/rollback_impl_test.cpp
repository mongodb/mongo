/**
 *    Copyright 2017 MongoDB Inc.
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

#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/rollback_impl.h"
#include "mongo/db/repl/rollback_test_fixture.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"
#include <boost/optional.hpp>

namespace {

using namespace mongo;
using namespace mongo::repl;

NamespaceString nss("local.oplog.rs");

class StorageInterfaceRollback : public StorageInterfaceImpl {
public:
    void setStableTimestamp(ServiceContext* serviceCtx, Timestamp snapshotName) override {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _stableTimestamp = snapshotName;
    }

    /**
     * If '_recoverToTimestampStatus' is non-empty, returns it. If '_recoverToTimestampStatus' is
     * empty, updates '_currTimestamp' to be equal to '_stableTimestamp' and returns an OK status.
     */
    Status recoverToStableTimestamp(ServiceContext* serviceCtx) override {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        if (_recoverToTimestampStatus) {
            return _recoverToTimestampStatus.get();
        } else {
            _currTimestamp = _stableTimestamp;
            return Status::OK();
        }
    }

    void setRecoverToTimestampStatus(Status status) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _recoverToTimestampStatus = status;
    }

    void setCurrentTimestamp(Timestamp ts) {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _currTimestamp = ts;
    }

    Timestamp getCurrentTimestamp() {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _currTimestamp;
    }

private:
    mutable stdx::mutex _mutex;

    Timestamp _stableTimestamp;

    // Used to mock the behavior of 'recoverToStableTimestamp'. Upon calling
    // 'recoverToStableTimestamp', the 'currTimestamp' should be set to the current
    // '_stableTimestamp' value. Can be viewed as mock version of replication's 'lastApplied'
    // optime.
    Timestamp _currTimestamp;

    // A Status value which, if set, will be returned by the 'recoverToStableTimestamp' function, in
    // order to simulate the error case for that function. Defaults to boost::none.
    boost::optional<Status> _recoverToTimestampStatus = boost::none;
};

/**
 * Unit test for rollback implementation introduced in 3.6.
 */
class RollbackImplTest : public RollbackTest {
public:
    /**
     * Implementation of RollbackImpl::Listener used by this class.
     */
    class Listener;

private:
    void setUp() override;
    void tearDown() override;

    friend class RollbackImplTest::Listener;

protected:
    std::unique_ptr<StorageInterfaceRollback> _storageInterface;
    std::unique_ptr<OplogInterfaceLocal> _localOplog;
    std::unique_ptr<OplogInterfaceMock> _remoteOplog;
    std::unique_ptr<RollbackImpl> _rollback;

    bool _transitionedToRollback = false;
    stdx::function<void()> _onTransitionToRollbackFn = [this]() { _transitionedToRollback = true; };

    bool _recoveredToStableTimestamp = false;
    stdx::function<void()> _onRecoverToStableTimestampFn = [this]() {
        _recoveredToStableTimestamp = true;
    };

    bool _recoveredFromOplog = false;
    stdx::function<void()> _onRecoverFromOplogFn = [this]() { _recoveredFromOplog = true; };

    Timestamp _commonPointFound;
    stdx::function<void(Timestamp commonPoint)> _onCommonPointFoundFn =
        [this](Timestamp commonPoint) { _commonPointFound = commonPoint; };

    bool _triggeredOpObserver = false;
    stdx::function<void(const OpObserver::RollbackObserverInfo& rbInfo)> _onRollbackOpObserverFn =
        [this](const OpObserver::RollbackObserverInfo& rbInfo) {};

    std::unique_ptr<Listener> _listener;
};

void RollbackImplTest::setUp() {
    RollbackTest::setUp();

    // Set up test-specific storage interface.
    _storageInterface = stdx::make_unique<StorageInterfaceRollback>();

    _localOplog = stdx::make_unique<OplogInterfaceLocal>(_opCtx.get(),
                                                         NamespaceString::kRsOplogNamespace.ns());
    _remoteOplog = stdx::make_unique<OplogInterfaceMock>();
    _listener = stdx::make_unique<Listener>(this);
    _rollback = stdx::make_unique<RollbackImpl>(_localOplog.get(),
                                                _remoteOplog.get(),
                                                _storageInterface.get(),
                                                _replicationProcess.get(),
                                                _coordinator,
                                                _listener.get());

    createOplog(_opCtx.get());
}

void RollbackImplTest::tearDown() {
    _rollback = {};
    _localOplog = {};
    _remoteOplog = {};
    _listener = {};
    RollbackTest::tearDown();
}

class RollbackImplTest::Listener : public RollbackImpl::Listener {
public:
    Listener(RollbackImplTest* test) : _test(test) {}

    void onTransitionToRollback() noexcept {
        _test->_onTransitionToRollbackFn();
    }

    void onCommonPointFound(Timestamp commonPoint) noexcept {
        _test->_onCommonPointFoundFn(commonPoint);
    }

    void onRecoverToStableTimestamp() noexcept {
        _test->_onRecoverToStableTimestampFn();
    }

    void onRecoverFromOplog() noexcept {
        _test->_onRecoverFromOplogFn();
    }

    void onRollbackOpObserver(const OpObserver::RollbackObserverInfo& rbInfo) noexcept {
        _test->_onRollbackOpObserverFn(rbInfo);
    }

private:
    RollbackImplTest* _test;
};

/**
 * Helper functions to make simple oplog entries with timestamps, terms, and hashes.
 */
BSONObj makeOp(OpTime time, long long hash) {
    return BSON("ts" << time.getTimestamp() << "h" << hash << "t" << time.getTerm() << "op"
                     << "i"
                     << "o"
                     << BSONObj()
                     << "ns"
                     << "test.coll"
                     << "ui"
                     << UUID::gen());
}

BSONObj makeOp(int count) {
    return makeOp(OpTime(Timestamp(count, count), count), count);
}

/**
 * Helper functions to make pairs of oplog entries and recordIds for the OplogInterfaceMock used
 * to mock out the local and remote oplogs.
 */
int recordId = 0;
OplogInterfaceMock::Operation makeOpAndRecordId(const BSONObj& op) {
    return std::make_pair(op, RecordId(++recordId));
}

OplogInterfaceMock::Operation makeOpAndRecordId(OpTime time, long long hash) {
    return makeOpAndRecordId(makeOp(time, hash));
}

OplogInterfaceMock::Operation makeOpAndRecordId(int count) {
    return makeOpAndRecordId(makeOp(count));
}

TEST_F(RollbackImplTest, TestFixtureSetUpInitializesStorageEngine) {
    auto serviceContext = _serviceContextMongoDTest.getServiceContext();
    ASSERT_TRUE(serviceContext);
    ASSERT_TRUE(serviceContext->getGlobalStorageEngine());
}

TEST_F(RollbackImplTest, RollbackReturnsNotSecondaryWhenFailingToTransitionToRollback) {
    _coordinator->failSettingFollowerMode(MemberState::RS_ROLLBACK, ErrorCodes::NotSecondary);

    ASSERT_EQUALS(ErrorCodes::NotSecondary, _rollback->runRollback(_opCtx.get()));
}

TEST_F(RollbackImplTest, RollbackReturnsInvalidSyncSourceWhenNoRemoteOplog) {
    ASSERT_OK(_insertOplogEntry(makeOp(1)));

    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource, _rollback->runRollback(_opCtx.get()));

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);
}

TEST_F(RollbackImplTest, RollbackReturnsOplogStartMissingWhenNoLocalOplog) {
    _remoteOplog->setOperations({makeOpAndRecordId(1)});

    ASSERT_EQUALS(ErrorCodes::OplogStartMissing, _rollback->runRollback(_opCtx.get()));

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);
}

TEST_F(RollbackImplTest, RollbackReturnsNoMatchingDocumentWhenNoCommonPoint) {
    _remoteOplog->setOperations({makeOpAndRecordId(1)});
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, _rollback->runRollback(_opCtx.get()));

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);
}

TEST_F(RollbackImplTest, RollbackPersistsDocumentAfterCommonPointToOplogTruncateAfterPoint) {
    auto commonPoint = makeOpAndRecordId(2);
    _remoteOplog->setOperations({commonPoint});
    ASSERT_OK(_insertOplogEntry(commonPoint.first));

    auto nextTime = 3;
    ASSERT_OK(_insertOplogEntry(makeOp(nextTime)));

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // Check that the common point was saved.
    auto truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(nextTime, nextTime), truncateAfterPoint);
}

TEST_F(RollbackImplTest, RollbackIncrementsRollbackID) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    // Get the initial rollback id.
    int initRollbackId = _replicationProcess->getRollbackID();

    // Run rollback.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // Check that the rollback id was incremented.
    int newRollbackId = _replicationProcess->getRollbackID();
    ASSERT_EQUALS(initRollbackId + 1, newRollbackId);
}

TEST_F(RollbackImplTest, RollbackCallsRecoverToStableTimestamp) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    auto stableTimestamp = Timestamp(10, 0);
    auto currTimestamp = Timestamp(20, 0);

    _storageInterface->setStableTimestamp(nullptr, stableTimestamp);
    _storageInterface->setCurrentTimestamp(currTimestamp);

    // Check the current timestamp.
    ASSERT_EQUALS(currTimestamp, _storageInterface->getCurrentTimestamp());

    // Run rollback.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // Make sure "recover to timestamp" occurred by checking that the current timestamp was set back
    // to the stable timestamp.
    ASSERT_EQUALS(stableTimestamp, _storageInterface->getCurrentTimestamp());
}

TEST_F(RollbackImplTest, RollbackReturnsBadStatusIfRecoverToStableTimestampFails) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    auto stableTimestamp = Timestamp(10, 0);
    auto currTimestamp = Timestamp(20, 0);
    _storageInterface->setStableTimestamp(nullptr, stableTimestamp);
    _storageInterface->setCurrentTimestamp(currTimestamp);

    // Make it so that the 'recoverToStableTimestamp' method will fail.
    auto recoverToTimestampStatus =
        Status(ErrorCodes::InternalError, "recoverToStableTimestamp failed.");
    _storageInterface->setRecoverToTimestampStatus(recoverToTimestampStatus);

    // Check the current timestamp.
    ASSERT_EQUALS(currTimestamp, _storageInterface->getCurrentTimestamp());

    // Run rollback.
    auto rollbackStatus = _rollback->runRollback(_opCtx.get());

    // Make sure rollback failed, and didn't execute the recover to timestamp logic.
    ASSERT_EQUALS(recoverToTimestampStatus, rollbackStatus);
    ASSERT_EQUALS(currTimestamp, _storageInterface->getCurrentTimestamp());

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);
}

TEST_F(RollbackImplTest, RollbackReturnsBadStatusIfIncrementRollbackIDFails) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    // Delete the rollback id collection.
    auto rollbackIdNss = NamespaceString(_storageInterface->kDefaultRollbackIdNamespace);
    ASSERT_OK(_storageInterface->dropCollection(_opCtx.get(), rollbackIdNss));

    // Run rollback.
    auto status = _rollback->runRollback(_opCtx.get());

    // Check that a bad status was returned since incrementing the rollback id should have
    // failed.
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status.code());

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);
}

TEST_F(RollbackImplTest, RollbackCallsRecoverFromOplog) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    // Run rollback.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // Make sure oplog recovery was executed.
    ASSERT(_recoveredFromOplog);
}

TEST_F(RollbackImplTest, RollbackSkipsRecoverFromOplogWhenShutdownEarly) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    _onRecoverToStableTimestampFn = [this]() {
        _recoveredToStableTimestamp = true;
        _rollback->shutdown();
    };

    // Run rollback.
    auto status = _rollback->runRollback(_opCtx.get());

    // Make sure shutdown occurred before oplog recovery.
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _rollback->runRollback(_opCtx.get()));
    ASSERT(_recoveredToStableTimestamp);
    ASSERT_FALSE(_recoveredFromOplog);

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);
}

TEST_F(RollbackImplTest, RollbackSucceeds) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT_EQUALS(Timestamp(1, 1), _commonPointFound);
}

DEATH_TEST_F(RollbackImplTest,
             RollbackTriggersFatalAssertionOnFailingToTransitionFromRollbackToSecondary,
             "Failed to transition into SECONDARY; expected to be in state ROLLBACK; found self in "
             "ROLLBACK") {
    _coordinator->failSettingFollowerMode(MemberState::RS_SECONDARY, ErrorCodes::IllegalOperation);

    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    auto status = _rollback->runRollback(_opCtx.get());
    unittest::log() << "Mongod did not crash. Status: " << status;
    MONGO_UNREACHABLE;
}

TEST_F(RollbackImplTest, RollbackSkipsTransitionToRollbackWhenShutDownImmediately) {
    _rollback->shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _rollback->runRollback(_opCtx.get()));
    ASSERT_FALSE(_transitionedToRollback);
}

TEST_F(RollbackImplTest, RollbackSkipsCommonPointWhenShutDownEarly) {
    _onTransitionToRollbackFn = [this]() {
        _transitionedToRollback = true;
        _rollback->shutdown();
    };

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _rollback->runRollback(_opCtx.get()));
    ASSERT(_transitionedToRollback);
    ASSERT_EQUALS(Timestamp(0, 0), _commonPointFound);
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);
}

TEST_F(RollbackImplTest, RollbackSkipsTriggerOpObserverWhenShutDownEarly) {
    _onRecoverFromOplogFn = [this]() {
        _recoveredFromOplog = true;
        _rollback->shutdown();
    };

    // Dummy rollback ops.
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _rollback->runRollback(_opCtx.get()));
    ASSERT(_recoveredFromOplog);
    ASSERT_FALSE(_triggeredOpObserver);
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);
}

/**
 * Fixture to help test that rollback records the correct information in its RollbackObserverInfo
 * struct.
 */
class RollbackImplObserverInfoTest : public RollbackImplTest {
public:
    /**
     * Simulates the rollback of a given sequence of operations. Returns the status of the rollback
     * process.
     */
    Status rollbackOps(const OplogInterfaceMock::Operations& ops) {
        auto commonOp = makeOpAndRecordId(1);
        _remoteOplog->setOperations({commonOp});
        ASSERT_OK(_insertOplogEntry(commonOp.first));
        for (auto it = ops.rbegin(); it != ops.rend(); it++) {
            ASSERT_OK(_insertOplogEntry(it->first));
        }
        _onRollbackOpObserverFn = [&](const OpObserver::RollbackObserverInfo& rbInfo) {
            _rbInfo = rbInfo;
        };

        // Run the rollback.
        return _rollback->runRollback(_opCtx.get());
    }

    BSONObj makeSessionOp(NamespaceString nss, UUID sessionId, TxnNumber txnNum) {
        auto uuid = UUID::gen();
        BSONObjBuilder bob;
        bob.append("ts", Timestamp(2, 1));
        bob.append("h", 1LL);
        bob.append("op", "i");
        uuid.appendToBuilder(&bob, "ui");
        bob.append("ns", nss.ns());
        bob.append("o", BSON("_id" << 1));
        bob.append("lsid",
                   BSON("id" << sessionId << "uid"
                             << BSONBinData(std::string(32, 'x').data(), 32, BinDataGeneral)));
        bob.append("txnNumber", txnNum);
        return bob.obj();
    }

protected:
    OpObserver::RollbackObserverInfo _rbInfo;
};

TEST_F(RollbackImplObserverInfoTest, NamespacesForOpsExtractsNamespaceOfInsertOplogEntry) {
    auto insertNss = NamespaceString("test", "coll");
    auto ts = Timestamp(2, 2);
    auto insertOp = makeCRUDOp(
        OpTypeEnum::kInsert, ts, UUID::gen(), insertNss.ns(), BSON("_id" << 1), boost::none, 2);

    std::set<NamespaceString> expectedNamespaces = {insertNss};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(insertOp.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest, NamespacesForOpsExtractsNamespaceOfUpdateOplogEntry) {
    auto updateNss = NamespaceString("test", "coll");
    auto ts = Timestamp(2, 2);
    auto o = BSON("$set" << BSON("x" << 2));
    auto updateOp =
        makeCRUDOp(OpTypeEnum::kUpdate, ts, UUID::gen(), updateNss.ns(), o, BSON("_id" << 1), 2);

    std::set<NamespaceString> expectedNamespaces = {updateNss};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(updateOp.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest, NamespacesForOpsExtractsNamespaceOfDeleteOplogEntry) {
    auto deleteNss = NamespaceString("test", "coll");
    auto ts = Timestamp(2, 2);
    auto deleteOp = makeCRUDOp(
        OpTypeEnum::kDelete, ts, UUID::gen(), deleteNss.ns(), BSON("_id" << 1), boost::none, 2);

    std::set<NamespaceString> expectedNamespaces = {deleteNss};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(deleteOp.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest, NamespacesForOpsIgnoresNamespaceOfNoopOplogEntry) {
    auto noopNss = NamespaceString("test", "coll");
    auto ts = Timestamp(2, 2);
    auto noop =
        makeCRUDOp(OpTypeEnum::kNoop, ts, UUID::gen(), noopNss.ns(), BSONObj(), boost::none, 2);

    std::set<NamespaceString> expectedNamespaces = {};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(noop.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesForOpsExtractsNamespaceOfCreateCollectionOplogEntry) {
    auto nss = NamespaceString("test", "coll");
    auto cmdOp = makeCommandOp(Timestamp(2, 2),
                               UUID::gen(),
                               nss.getCommandNS().toString(),
                               BSON("create" << nss.coll()),
                               2);
    std::set<NamespaceString> expectedNamespaces = {nss};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest, NamespacesForOpsExtractsNamespaceOfDropCollectionOplogEntry) {
    auto nss = NamespaceString("test", "coll");
    auto cmdOp = makeCommandOp(
        Timestamp(2, 2), UUID::gen(), nss.getCommandNS().toString(), BSON("drop" << nss.coll()), 2);

    std::set<NamespaceString> expectedNamespaces = {nss};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest, NamespacesForOpsExtractsNamespaceOfCreateIndexOplogEntry) {
    auto nss = NamespaceString("test", "coll");
    auto indexObj = BSON("createIndexes" << nss.coll() << "ns" << nss.toString() << "v"
                                         << static_cast<int>(IndexDescriptor::IndexVersion::kV2)
                                         << "key"
                                         << "x"
                                         << "name"
                                         << "x_1");
    auto cmdOp =
        makeCommandOp(Timestamp(2, 2), UUID::gen(), nss.getCommandNS().toString(), indexObj, 2);

    std::set<NamespaceString> expectedNamespaces = {nss};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest, NamespacesForOpsExtractsNamespaceOfDropIndexOplogEntry) {
    auto nss = NamespaceString("test", "coll");
    auto cmdOp = makeCommandOp(Timestamp(2, 2),
                               UUID::gen(),
                               nss.getCommandNS().toString(),
                               BSON("dropIndexes" << nss.coll() << "index"
                                                  << "x_1"),
                               2);
    std::set<NamespaceString> expectedNamespaces = {nss};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesForOpsExtractsNamespacesOfRenameCollectionOplogEntry) {
    auto fromNss = NamespaceString("test", "source");
    auto toNss = NamespaceString("test", "dest");

    auto cmdObj = BSON("renameCollection" << fromNss.ns() << "to" << toNss.ns());
    auto cmdOp =
        makeCommandOp(Timestamp(2, 2), UUID::gen(), fromNss.getCommandNS().ns(), cmdObj, 2);

    std::set<NamespaceString> expectedNamespaces = {fromNss, toNss};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest, NamespacesForOpsIgnoresNamespaceOfDropDatabaseOplogEntry) {
    auto nss = NamespaceString("test", "coll");
    auto cmdObj = BSON("dropDatabase" << 1);
    auto cmdOp = makeCommandOp(Timestamp(2, 2), boost::none, nss.getCommandNS().ns(), cmdObj, 2);

    std::set<NamespaceString> expectedNamespaces = {};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest, NamespacesForOpsExtractsNamespacesOfCollModOplogEntry) {
    auto nss = NamespaceString("test", "coll");
    auto cmdObj = BSON("collMod" << nss.coll() << "validationLevel"
                                 << "off");
    auto cmdOp = makeCommandOp(Timestamp(2, 2), UUID::gen(), nss.getCommandNS().ns(), cmdObj, 2);

    std::set<NamespaceString> expectedNamespaces = {nss};
    auto namespaces =
        unittest::assertGet(_rollback->_namespacesForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
}

TEST_F(RollbackImplObserverInfoTest, NamespacesForOpsFailsOnUnsupportedOplogEntry) {
    // 'convertToCapped' is not supported in rollback.
    auto convertToCappedOp =
        makeCommandOp(Timestamp(2, 2), boost::none, "test.$cmd", BSON("convertToCapped" << 1), 2);

    auto status =
        _rollback->_namespacesForOp_forTest(OplogEntry(convertToCappedOp.first)).getStatus();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status);

    // 'emptycapped' is not supported in rollback.
    auto emptycappedOp =
        makeCommandOp(Timestamp(2, 2), boost::none, "test.$cmd", BSON("emptycapped" << 1), 2);

    status = _rollback->_namespacesForOp_forTest(OplogEntry(emptycappedOp.first)).getStatus();
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, status);
}

DEATH_TEST_F(RollbackImplObserverInfoTest,
             NamespacesForOpsInvariantsOnApplyOpsOplogEntry,
             "_namespacesForOp does not handle 'applyOps' oplog entries.") {
    // Add one sub-op.
    auto createNss = NamespaceString("test", "createColl");
    auto createOp = makeCommandOp(Timestamp(2, 2),
                                  UUID::gen(),
                                  createNss.getCommandNS().toString(),
                                  BSON("create" << createNss.coll()),
                                  2);

    // Create the applyOps command object.
    BSONArrayBuilder subops;
    subops.append(createOp.first);
    auto applyOpsCmdOp = makeCommandOp(
        Timestamp(2, 2), boost::none, "admin.$cmd", BSON("applyOps" << subops.arr()), 2);

    auto status = _rollback->_namespacesForOp_forTest(OplogEntry(applyOpsCmdOp.first));
    unittest::log() << "Mongod did not crash. Status: " << status.getStatus();
    MONGO_UNREACHABLE;
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsNamespacesOfApplyOpsOplogEntry) {

    // Add a few different sub-ops from different namespaces to make sure they all get recorded.
    auto createNss = NamespaceString("test", "createColl");
    auto createOp = makeCommandOp(Timestamp(2, 2),
                                  UUID::gen(),
                                  createNss.getCommandNS().toString(),
                                  BSON("create" << createNss.coll()),
                                  2);

    auto dropNss = NamespaceString("test", "dropColl");
    auto dropOp = makeCommandOp(Timestamp(2, 2),
                                UUID::gen(),
                                dropNss.getCommandNS().toString(),
                                BSON("drop" << dropNss.coll()),
                                2);

    auto collModNss = NamespaceString("test", "collModColl");
    auto collModOp = makeCommandOp(Timestamp(2, 2),
                                   UUID::gen(),
                                   collModNss.getCommandNS().ns(),
                                   BSON("collMod" << collModNss.coll() << "validationLevel"
                                                  << "off"),
                                   2);

    // Create the applyOps command object.
    BSONArrayBuilder subops;
    subops.append(createOp.first);
    subops.append(dropOp.first);
    subops.append(collModOp.first);
    auto applyOpsCmdOp = makeCommandOp(
        Timestamp(2, 2), boost::none, "admin.$cmd", BSON("applyOps" << subops.arr()), 2);

    ASSERT_OK(rollbackOps({applyOpsCmdOp}));
    std::set<NamespaceString> expectedNamespaces = {createNss, dropNss, collModNss};
    ASSERT(expectedNamespaces == _rbInfo.rollbackNamespaces);
}

TEST_F(RollbackImplObserverInfoTest, RollbackFailsOnMalformedApplyOpsOplogEntry) {
    // Make the argument to the 'applyOps' command an object instead of an array. This should cause
    // rollback to fail, since applyOps expects an array of ops.
    auto applyOpsCmdOp = makeCommandOp(Timestamp(2, 2),
                                       boost::none,
                                       "admin.$cmd",
                                       BSON("applyOps" << BSON("not"
                                                               << "array")),
                                       2);

    auto status = rollbackOps({applyOpsCmdOp});
    ASSERT_NOT_OK(status);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsNamespaceOfSingleOplogEntry) {
    auto nss = NamespaceString("test", "coll");
    auto insertOp = makeCRUDOp(OpTypeEnum::kInsert,
                               Timestamp(2, 2),
                               UUID::gen(),
                               nss.ns(),
                               BSON("_id" << 1),
                               boost::none,
                               2);
    ASSERT_OK(rollbackOps({insertOp}));
    std::set<NamespaceString> expectedNamespaces = {nss};
    ASSERT(expectedNamespaces == _rbInfo.rollbackNamespaces);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsMultipleNamespacesOfOplogEntries) {
    auto makeInsertOp = [&](NamespaceString nss, Timestamp ts, int recordId) {
        return makeCRUDOp(OpTypeEnum::kInsert,
                          ts,
                          UUID::gen(),
                          nss.ns(),
                          BSON("_id" << 1),
                          boost::none,
                          recordId);
    };

    auto nss1 = NamespaceString("test", "coll1");
    auto nss2 = NamespaceString("test", "coll2");
    auto nss3 = NamespaceString("test", "coll3");

    auto insertOp1 = makeInsertOp(nss1, Timestamp(2, 1), 2);
    auto insertOp2 = makeInsertOp(nss2, Timestamp(3, 1), 3);
    auto insertOp3 = makeInsertOp(nss3, Timestamp(4, 1), 4);

    ASSERT_OK(rollbackOps({insertOp3, insertOp2, insertOp1}));
    std::set<NamespaceString> expectedNamespaces = {nss1, nss2, nss3};
    ASSERT(expectedNamespaces == _rbInfo.rollbackNamespaces);
}

DEATH_TEST_F(RollbackImplObserverInfoTest,
             RollbackFailsOnUnknownOplogEntryCommandType,
             "Unknown oplog entry command type") {
    // Create a command of an unknown type.
    auto unknownCmdOp =
        makeCommandOp(Timestamp(2, 2), boost::none, "admin.$cmd", BSON("unknownCommand" << 1), 2);

    auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    ASSERT_OK(_insertOplogEntry(unknownCmdOp.first));

    auto status = _rollback->runRollback(_opCtx.get());
    unittest::log() << "Mongod did not crash. Status: " << status;
    MONGO_UNREACHABLE;
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsSessionIdFromOplogEntry) {

    NamespaceString nss("test.coll");
    auto sessionId = UUID::gen();
    auto sessionOpObj = makeSessionOp(nss, sessionId, 1L);
    auto sessionOp = std::make_pair(sessionOpObj, RecordId(recordId));

    // Run the rollback and make sure the correct session id was recorded.
    ASSERT_OK(rollbackOps({sessionOp}));
    std::set<UUID> expectedSessionIds = {sessionId};
    ASSERT(expectedSessionIds == _rbInfo.rollbackSessionIds);
}

TEST_F(RollbackImplObserverInfoTest,
       RollbackDoesntRecordSessionIdFromOplogEntryWithoutSessionInfo) {
    auto nss = NamespaceString("test", "coll");
    auto insertOp = makeCRUDOp(OpTypeEnum::kInsert,
                               Timestamp(2, 2),
                               UUID::gen(),
                               nss.ns(),
                               BSON("_id" << 1),
                               boost::none,
                               2);
    ASSERT_OK(rollbackOps({insertOp}));
    ASSERT(_rbInfo.rollbackSessionIds.empty());
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsSessionIdFromApplyOpsSubOp) {

    NamespaceString nss("test.coll");
    auto sessionId = UUID::gen();
    auto sessionOpObj = makeSessionOp(nss, sessionId, 1L);

    // Create the applyOps command object.
    BSONArrayBuilder subops;
    subops.append(sessionOpObj);
    auto applyOpsCmdOp = makeCommandOp(
        Timestamp(2, 2), boost::none, "admin.$cmd", BSON("applyOps" << subops.arr()), 2);

    // Run the rollback and make sure the correct session id was recorded.
    ASSERT_OK(rollbackOps({applyOpsCmdOp}));
    std::set<UUID> expectedSessionIds = {sessionId};
    ASSERT(expectedSessionIds == _rbInfo.rollbackSessionIds);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsShardIdentityRollback) {
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    auto nss = NamespaceString::kServerConfigurationNamespace;
    auto insertShardIdOp = makeCRUDOp(OpTypeEnum::kInsert,
                                      Timestamp(2, 2),
                                      UUID::gen(),
                                      nss.ns(),
                                      BSON("_id" << ShardIdentityType::IdName),
                                      boost::none,
                                      2);

    ASSERT_OK(rollbackOps({insertShardIdOp}));
    ASSERT(_rbInfo.shardIdentityRolledBack);
}


TEST_F(RollbackImplObserverInfoTest, RollbackDoesntRecordShardIdentityRollbackForNormalDocument) {
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    auto nss = NamespaceString::kServerConfigurationNamespace;
    auto deleteOp = makeCRUDOp(OpTypeEnum::kDelete,
                               Timestamp(2, 2),
                               UUID::gen(),
                               nss.ns(),
                               BSON("_id"
                                    << "not_the_shard_id_document"),
                               boost::none,
                               2);
    ASSERT_OK(rollbackOps({deleteOp}));
    ASSERT_FALSE(_rbInfo.shardIdentityRolledBack);
}
}  // namespace
