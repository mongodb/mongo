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

#include "mongo/db/repl/rollback_test_fixture.h"

#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/rollback_impl.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
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
    std::unique_ptr<OplogInterfaceMock> _localOplog;
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

    std::unique_ptr<Listener> _listener;
};

void RollbackImplTest::setUp() {
    RollbackTest::setUp();

    // Set up test-specific storage interface.
    _storageInterface = stdx::make_unique<StorageInterfaceRollback>();

    _localOplog = stdx::make_unique<OplogInterfaceMock>();
    _remoteOplog = stdx::make_unique<OplogInterfaceMock>();
    _listener = stdx::make_unique<Listener>(this);
    _rollback = stdx::make_unique<RollbackImpl>(_localOplog.get(),
                                                _remoteOplog.get(),
                                                _storageInterface.get(),
                                                _replicationProcess.get(),
                                                _coordinator,
                                                _listener.get());
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

private:
    RollbackImplTest* _test;
};

/**
 * Helper functions to make simple oplog entries with timestamps, terms, and hashes.
 */
BSONObj makeOp(OpTime time, long long hash) {
    return BSON("ts" << time.getTimestamp() << "h" << hash << "t" << time.getTerm() << "op"
                     << "i"
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
    _localOplog->setOperations({makeOpAndRecordId(1)});

    ASSERT_EQUALS(ErrorCodes::InvalidSyncSource, _rollback->runRollback(_opCtx.get()));
}

TEST_F(RollbackImplTest, RollbackReturnsOplogStartMissingWhenNoLocalOplog) {
    _remoteOplog->setOperations({makeOpAndRecordId(1)});

    ASSERT_EQUALS(ErrorCodes::OplogStartMissing, _rollback->runRollback(_opCtx.get()));
}

TEST_F(RollbackImplTest, RollbackReturnsNoMatchingDocumentWhenNoCommonPoint) {
    _remoteOplog->setOperations({makeOpAndRecordId(1)});
    _localOplog->setOperations({makeOpAndRecordId(2)});

    ASSERT_EQUALS(ErrorCodes::NoMatchingDocument, _rollback->runRollback(_opCtx.get()));
}

TEST_F(RollbackImplTest, RollbackPersistsCommonPointToOplogTruncateAfterPoint) {
    _remoteOplog->setOperations({makeOpAndRecordId(2)});
    _localOplog->setOperations({makeOpAndRecordId(2)});

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // Check that the common point was saved.
    auto truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(2, 2), truncateAfterPoint);
}

TEST_F(RollbackImplTest, RollbackIncrementsRollbackID) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    _localOplog->setOperations({op});

    // Get the initial rollback id.
    int initRollbackId = unittest::assertGet(_replicationProcess->getRollbackID(_opCtx.get()));

    // Run rollback.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // Check that the rollback id was incremented.
    int newRollbackId = unittest::assertGet(_replicationProcess->getRollbackID(_opCtx.get()));
    ASSERT_EQUALS(initRollbackId + 1, newRollbackId);
}

TEST_F(RollbackImplTest, RollbackCallsRecoverToStableTimestamp) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    _localOplog->setOperations({op});

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
    _localOplog->setOperations({op});

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
}

TEST_F(RollbackImplTest, RollbackReturnsBadStatusIfIncrementRollbackIDFails) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    _localOplog->setOperations({op});

    // Delete the rollback id collection.
    auto rollbackIdNss = NamespaceString(_storageInterface->kDefaultRollbackIdNamespace);
    ASSERT_OK(_storageInterface->dropCollection(_opCtx.get(), rollbackIdNss));

    // Run rollback.
    auto status = _rollback->runRollback(_opCtx.get());

    // Check that a bad status was returned since incrementing the rollback id should have failed.
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status.code());
}

TEST_F(RollbackImplTest, RollbackCallsRecoverFromOplog) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    _localOplog->setOperations({op});

    // Run rollback.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // Make sure oplog recovery was executed.
    ASSERT(_recoveredFromOplog);
}

TEST_F(RollbackImplTest, RollbackSkipsRecoverFromOplogWhenShutdownEarly) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    _localOplog->setOperations({op});

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
}

TEST_F(RollbackImplTest, RollbackSucceeds) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    _localOplog->setOperations({op});

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT_EQUALS(Timestamp(1, 1), _commonPointFound);
}

DEATH_TEST_F(RollbackImplTest,
             RollbackTriggersFatalAssertionOnDetectingShardIdentityDocumentRollback,
             "shardIdentity document rollback detected.  Shutting down to clear in-memory sharding "
             "state.  Restarting this process should safely return it to a healthy state") {
    ASSERT_FALSE(ShardIdentityRollbackNotifier::get(_opCtx.get())->didRollbackHappen());
    ShardIdentityRollbackNotifier::get(_opCtx.get())->recordThatRollbackHappened();
    ASSERT_TRUE(ShardIdentityRollbackNotifier::get(_opCtx.get())->didRollbackHappen());

    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    _localOplog->setOperations({op});
    auto status = _rollback->runRollback(_opCtx.get());
    unittest::log() << "Mongod did not crash. Status: " << status;
    MONGO_UNREACHABLE;
}

DEATH_TEST_F(RollbackImplTest,
             RollbackTriggersFatalAssertionOnFailingToTransitionFromRollbackToSecondary,
             "Failed to transition into SECONDARY; expected to be in state ROLLBACK; found self in "
             "ROLLBACK") {
    _coordinator->failSettingFollowerMode(MemberState::RS_SECONDARY, ErrorCodes::IllegalOperation);

    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    _localOplog->setOperations({op});
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
}

}  // namespace
