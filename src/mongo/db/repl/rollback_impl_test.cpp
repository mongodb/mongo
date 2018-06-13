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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kReplicationRollback

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/rollback_impl.h"
#include "mongo/db/repl/rollback_test_fixture.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

NamespaceString kOplogNSS("local.oplog.rs");
NamespaceString nss("test.coll");
std::string kGenericUUIDStr = "b4c66a44-c1ca-4d86-8d25-12e82fa2de5b";

BSONObj makeInsertOplogEntry(long long time, BSONObj obj, StringData ns, UUID uuid) {
    return BSON("ts" << Timestamp(time, time) << "h" << time << "t" << time << "op"
                     << "i"
                     << "o"
                     << obj
                     << "ns"
                     << ns
                     << "ui"
                     << uuid);
}

BSONObj makeUpdateOplogEntry(
    long long time, BSONObj query, BSONObj update, StringData ns, UUID uuid) {
    return BSON("ts" << Timestamp(time, time) << "h" << time << "t" << time << "op"
                     << "u"
                     << "ns"
                     << ns
                     << "ui"
                     << uuid
                     << "o2"
                     << query
                     << "o"
                     << BSON("$set" << update));
}

BSONObj makeDeleteOplogEntry(long long time, BSONObj id, StringData ns, UUID uuid) {
    return BSON("ts" << Timestamp(time, time) << "h" << time << "t" << time << "op"
                     << "d"
                     << "ns"
                     << ns
                     << "ui"
                     << uuid
                     << "o"
                     << id);
}

class RollbackImplForTest final : public RollbackImpl {
public:
    using RollbackImpl::RollbackImpl;

    static const std::vector<BSONObj> kEmptyVector;

    /**
     * Returns a reference to a vector containing all of the BSONObjs deleted from the namespace
     * represented by 'uuid', or kEmptyVector if that namespace wasn't found in '_uuidToObjsMap'.
     */
    const std::vector<BSONObj>& docsDeletedForNamespace_forTest(UUID uuid) const& final {
        auto iter = _uuidToObjsMap.find(uuid);
        if (iter == _uuidToObjsMap.end()) {
            return kEmptyVector;
        }
        return iter->second;
    }

protected:
    /**
     * Saves documents that would be deleted in '_uuidToObjsMap', rather than writing them out to a
     * file.
     */
    void _writeRollbackFileForNamespace(OperationContext* opCtx,
                                        UUID uuid,
                                        NamespaceString nss,
                                        const SimpleBSONObjUnorderedSet& idSet) final {
        log() << "Simulating writing a rollback file for namespace " << nss.ns() << " with uuid "
              << uuid;
        for (auto&& id : idSet) {
            log() << "Looking up " << id.jsonString();
            auto document = _findDocumentById(opCtx, uuid, nss, id.firstElement());
            if (document) {
                _uuidToObjsMap[uuid].push_back(*document);
            }
        }
        _listener->onRollbackFileWrittenForNamespace(std::move(uuid), std::move(nss));
    }

private:
    stdx::unordered_map<UUID, std::vector<BSONObj>, UUID::Hash> _uuidToObjsMap;
};

const std::vector<BSONObj> RollbackImplForTest::kEmptyVector;

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
    /**
     * Creates a new mock collection with name 'nss' via the StorageInterface and associates 'uuid'
     * with the new collection in the UUIDCatalog. There must not already exist a collection with
     * name 'nss'.
     */
    std::unique_ptr<Collection> _initializeCollection(OperationContext* opCtx,
                                                      UUID uuid,
                                                      const NamespaceString& nss) {
        // Create a new collection in the storage interface.
        CollectionOptions options;
        options.uuid = uuid;
        ASSERT_OK(_storageInterface->createCollection(opCtx, nss, options));

        // Initialize a mock collection.
        std::unique_ptr<Collection> coll =
            std::make_unique<Collection>(std::make_unique<CollectionMock>(nss));

        // Register the UUID to that collection in the UUIDCatalog.
        UUIDCatalog::get(opCtx).registerUUIDCatalogEntry(uuid, coll.get());
        return coll;
    }

    /**
     * Creates an oplog entry that represents the insertion of 'doc' into the namespace 'nss' with
     * UUID 'uuid', and inserts the document into the storage interface.
     *
     * Unless 'time' is explicitly given, this insert is timestamped with an arbitrary time that
     * monotonically increases with each successive call to this function.
     */
    void _insertDocAndGenerateOplogEntry(BSONObj doc,
                                         UUID uuid,
                                         NamespaceString nss,
                                         boost::optional<long> time = boost::none) {
        const auto optime = time.value_or(_counter++);
        ASSERT_OK(_insertOplogEntry(makeInsertOplogEntry(optime, doc, nss.ns(), uuid)));
        ASSERT_OK(_storageInterface->insertDocument(
            _opCtx.get(), nss, {doc, Timestamp(optime, optime)}, optime));
    }

    /**
     * Creates an oplog entry that represents the insertion of 'doc' into the namespace 'nss' with
     * UUID 'uuid', and inserts the document into the storage interface.
     *
     * Unless 'time' is explicitly given, this insert is timestamped with an arbitrary time that
     * monotonically increases with each successive call to this function.
     */
    OplogInterfaceMock::Operation _insertDocAndReturnOplogEntry(
        BSONObj doc, UUID uuid, NamespaceString nss, boost::optional<long> time = boost::none) {
        const auto optime = time.value_or(_counter++);
        ASSERT_OK(_storageInterface->insertDocument(
            _opCtx.get(), nss, {doc, Timestamp(optime, optime)}, optime));
        return std::make_pair(makeInsertOplogEntry(optime, doc, nss.ns(), uuid), RecordId(optime));
    }

    /**
     * Creates an oplog entry that represents updating an object matched by 'query' to be 'newDoc'
     * in the namespace 'nss, with UUID 'uuid'. It also inserts 'newDoc' into the storage interface.
     * This update is timestamped with the provided 'optime' or an arbitrary time that
     * monotonically increases with each successive call, if none is provided.
     */
    void _updateDocAndGenerateOplogEntry(BSONObj query,
                                         BSONObj newDoc,
                                         UUID uuid,
                                         NamespaceString nss,
                                         boost::optional<long> optime = boost::none) {
        const auto time = optime.value_or(_counter++);
        ASSERT_OK(_insertOplogEntry(makeUpdateOplogEntry(time, query, newDoc, nss.ns(), uuid)));
        ASSERT_OK(_storageInterface->insertDocument(
            _opCtx.get(), nss, {newDoc, Timestamp(time, time)}, time));
    }

    /**
     * Creates an oplog entry that represents deleting an object with _id 'id' in the namespace
     * 'nss' with UUID 'uuid'. This function also removes that document from the storage interface.
     * This delete is timestamped with the provided 'optime' or an arbitrary time that
     * monotonically increases with each successive call, if none is provided.
     */
    void _deleteDocAndGenerateOplogEntry(BSONElement id,
                                         UUID uuid,
                                         NamespaceString nss,
                                         boost::optional<long> optime = boost::none) {
        const auto time = optime.value_or(_counter++);
        ASSERT_OK(_insertOplogEntry(makeDeleteOplogEntry(time, id.wrap(), nss.ns(), uuid)));
        ASSERT_OK(_storageInterface->deleteById(_opCtx.get(), nss, id));
    }

    std::unique_ptr<OplogInterfaceLocal> _localOplog;
    std::unique_ptr<OplogInterfaceMock> _remoteOplog;
    std::unique_ptr<RollbackImplForTest> _rollback;

    bool _transitionedToRollback = false;
    stdx::function<void()> _onTransitionToRollbackFn = [this]() { _transitionedToRollback = true; };

    bool _recoveredToStableTimestamp = false;
    Timestamp _stableTimestamp;
    stdx::function<void(Timestamp)> _onRecoverToStableTimestampFn =
        [this](Timestamp stableTimestamp) {
            _recoveredToStableTimestamp = true;
            _stableTimestamp = stableTimestamp;
        };

    bool _recoveredFromOplog = false;
    stdx::function<void()> _onRecoverFromOplogFn = [this]() { _recoveredFromOplog = true; };

    Timestamp _commonPointFound;
    stdx::function<void(Timestamp commonPoint)> _onCommonPointFoundFn =
        [this](Timestamp commonPoint) { _commonPointFound = commonPoint; };

    Timestamp _truncatePoint;
    stdx::function<void(Timestamp truncatePoint)> _onSetOplogTruncateAfterPointFn =
        [this](Timestamp truncatePoint) { _truncatePoint = truncatePoint; };

    bool _triggeredOpObserver = false;
    stdx::function<void(const OpObserver::RollbackObserverInfo& rbInfo)> _onRollbackOpObserverFn =
        [this](const OpObserver::RollbackObserverInfo& rbInfo) { _triggeredOpObserver = true; };

    stdx::function<void(UUID, NamespaceString)> _onRollbackFileWrittenForNamespaceFn =
        [this](UUID, NamespaceString) {};

    std::unique_ptr<Listener> _listener;

    UUID kGenericUUID = unittest::assertGet(UUID::parse(kGenericUUIDStr));

private:
    long _counter = 100;
};

void RollbackImplTest::setUp() {
    RollbackTest::setUp();

    _localOplog = stdx::make_unique<OplogInterfaceLocal>(_opCtx.get(),
                                                         NamespaceString::kRsOplogNamespace.ns());
    _remoteOplog = stdx::make_unique<OplogInterfaceMock>();
    _listener = stdx::make_unique<Listener>(this);
    _rollback = stdx::make_unique<RollbackImplForTest>(_localOplog.get(),
                                                       _remoteOplog.get(),
                                                       _storageInterface,
                                                       _replicationProcess.get(),
                                                       _coordinator,
                                                       _listener.get());

    createOplog(_opCtx.get());
    serverGlobalParams.clusterRole = ClusterRole::None;
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

    void onTransitionToRollback() noexcept override {
        _test->_onTransitionToRollbackFn();
    }

    void onCommonPointFound(Timestamp commonPoint) noexcept override {
        _test->_onCommonPointFoundFn(commonPoint);
    }

    void onRollbackFileWrittenForNamespace(UUID uuid, NamespaceString nss) noexcept final {
        _test->_onRollbackFileWrittenForNamespaceFn(std::move(uuid), std::move(nss));
    }

    void onRecoverToStableTimestamp(Timestamp stableTimestamp) noexcept override {
        _test->_onRecoverToStableTimestampFn(stableTimestamp);
    }

    void onSetOplogTruncateAfterPoint(Timestamp truncatePoint) noexcept override {
        _test->_onSetOplogTruncateAfterPointFn(truncatePoint);
    }

    void onRecoverFromOplog() noexcept override {
        _test->_onRecoverFromOplogFn();
    }

    void onRollbackOpObserver(const OpObserver::RollbackObserverInfo& rbInfo) noexcept override {
        _test->_onRollbackOpObserverFn(rbInfo);
    }

private:
    RollbackImplTest* _test;
};

/**
 * Helper functions to make simple oplog entries with timestamps, terms, and hashes.
 */
BSONObj makeOp(OpTime time, long long hash) {
    auto kGenericUUID = unittest::assertGet(UUID::parse(kGenericUUIDStr));
    return BSON("ts" << time.getTimestamp() << "h" << hash << "t" << time.getTerm() << "op"
                     << "n"
                     << "o"
                     << BSONObj()
                     << "ns"
                     << nss.ns()
                     << "ui"
                     << kGenericUUID);
}

BSONObj makeOp(int count) {
    return makeOp(OpTime(Timestamp(count, count), count), count);
}

/**
 * Helper functions to make simple oplog entries with timestamps, terms, hashes, and wall clock
 * times.
 */
auto makeOpWithWallClockTime(long count, long long hash, long wallClockMillis) {
    auto kGenericUUID = unittest::assertGet(UUID::parse(kGenericUUIDStr));
    return BSON("ts" << Timestamp(count, count) << "h" << hash << "t" << (long long)count << "op"
                     << "n"
                     << "o"
                     << BSONObj()
                     << "ns"
                     << "top"
                     << "ui"
                     << kGenericUUID
                     << "wall"
                     << Date_t::fromMillisSinceEpoch(wallClockMillis));
};

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

/**
 * Asserts that the documents in the oplog have the given timestamps.
 */
void _assertDocsInOplog(OperationContext* opCtx, std::vector<int> timestamps) {
    std::vector<BSONObj> expectedOplog(timestamps.size());
    std::transform(timestamps.begin(), timestamps.end(), expectedOplog.begin(), [](int ts) {
        return makeOp(ts);
    });

    OplogInterfaceLocal oplog(opCtx, NamespaceString::kRsOplogNamespace.ns());
    auto iter = oplog.makeIterator();
    for (auto reverseIt = expectedOplog.rbegin(); reverseIt != expectedOplog.rend(); reverseIt++) {
        auto expectedTime = unittest::assertGet(OpTime::parseFromOplogEntry(*reverseIt));
        auto realTime = unittest::assertGet(
            OpTime::parseFromOplogEntry(unittest::assertGet(iter->next()).first));
        ASSERT_EQ(expectedTime, realTime);
    }
    ASSERT_EQUALS(ErrorCodes::CollectionIsEmpty, iter->next().getStatus());
}

TEST_F(RollbackImplTest, TestFixtureSetUpInitializesStorageEngine) {
    auto serviceContext = _serviceContextMongoDTest.getServiceContext();
    ASSERT_TRUE(serviceContext);
    ASSERT_TRUE(serviceContext->getStorageEngine());
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

TEST_F(RollbackImplTest, RollbackSucceedsIfRollbackPeriodIsWithinTimeLimit) {

    // The default limit is 1 day, so we make the difference be just under a day.
    auto commonPoint = makeOpAndRecordId(makeOpWithWallClockTime(1, 1, 5 * 1000));
    auto topOfOplog = makeOpAndRecordId(makeOpWithWallClockTime(2, 2, 60 * 60 * 24 * 1000));

    _remoteOplog->setOperations({commonPoint});
    ASSERT_OK(_insertOplogEntry(commonPoint.first));
    ASSERT_OK(_insertOplogEntry(topOfOplog.first));

    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    // Run rollback.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
}

TEST_F(RollbackImplTest, RollbackFailsIfRollbackPeriodIsTooLong) {

    // The default limit is 1 day, so we make the difference be 2 days.
    auto commonPoint = makeOpAndRecordId(makeOpWithWallClockTime(1, 1, 5 * 1000));
    auto topOfOplog = makeOpAndRecordId(makeOpWithWallClockTime(2, 2, 2 * 60 * 60 * 24 * 1000));

    _remoteOplog->setOperations({commonPoint});
    ASSERT_OK(_insertOplogEntry(commonPoint.first));
    ASSERT_OK(_insertOplogEntry(topOfOplog.first));

    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    // Run rollback.
    auto rollbackStatus = _rollback->runRollback(_opCtx.get());

    // Make sure rollback failed with an UnrecoverableRollbackError.
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, rollbackStatus.code());
}

TEST_F(RollbackImplTest, RollbackPersistsDocumentAfterCommonPointToOplogTruncateAfterPoint) {
    auto commonPoint = makeOpAndRecordId(2);
    _remoteOplog->setOperations({commonPoint});
    ASSERT_OK(_insertOplogEntry(commonPoint.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(2, 2));

    auto nextTime = 3;
    ASSERT_OK(_insertOplogEntry(makeOp(nextTime)));
    auto coll = _initializeCollection(_opCtx.get(), UUID::gen(), nss);

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT_EQUALS(_truncatePoint, Timestamp(3, 3));
}

TEST_F(RollbackImplTest, RollbackImplResetsOptimesFromOplogAfterRollback) {
    auto commonPoint = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonPoint});
    ASSERT_OK(_insertOplogEntry(commonPoint.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    auto nextTime = 2;
    ASSERT_OK(_insertOplogEntry(makeOp(nextTime)));

    // runRollback has not been called yet.
    ASSERT_FALSE(_recoveredToStableTimestamp);
    ASSERT_FALSE(_recoveredFromOplog);
    ASSERT_FALSE(_coordinator->lastOpTimesWereReset());

    _onRecoverToStableTimestampFn = [this](Timestamp stableTimestamp) {
        _recoveredToStableTimestamp = true;
        _stableTimestamp = stableTimestamp;
        ASSERT_FALSE(_coordinator->lastOpTimesWereReset());
    };

    _onRecoverFromOplogFn = [this]() {
        _recoveredFromOplog = true;
        ASSERT_FALSE(_coordinator->lastOpTimesWereReset());
    };

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // Verify that resetLastOptimesFromOplog is called towards the end of runRollback.
    ASSERT_TRUE(_coordinator->lastOpTimesWereReset());
}

TEST_F(RollbackImplTest, RollbackIncrementsRollbackID) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

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

    auto stableTimestamp = Timestamp(1, 1);
    auto currTimestamp = Timestamp(2, 2);

    _storageInterface->setStableTimestamp(nullptr, stableTimestamp);
    _storageInterface->setCurrentTimestamp(currTimestamp);

    // Check the current timestamp.
    ASSERT_EQUALS(currTimestamp, _storageInterface->getCurrentTimestamp());
    ASSERT_EQUALS(Timestamp(), _stableTimestamp);

    // Run rollback.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // Set the stable timestamp ahead to see that the current timestamp and the stable timestamp
    // we recovered to don't change.
    auto newTimestamp = Timestamp(3, 3);
    _storageInterface->setStableTimestamp(nullptr, newTimestamp);

    // Make sure "recover to timestamp" occurred by checking that the current timestamp was set back
    // to the stable timestamp.
    ASSERT_EQUALS(stableTimestamp, _storageInterface->getCurrentTimestamp());
    ASSERT_EQUALS(stableTimestamp, _stableTimestamp);
}

TEST_F(RollbackImplTest, RollbackReturnsBadStatusIfRecoverToStableTimestampFails) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    auto stableTimestamp = Timestamp(1, 1);
    auto currTimestamp = Timestamp(2, 2);
    _storageInterface->setStableTimestamp(nullptr, stableTimestamp);
    _storageInterface->setCurrentTimestamp(currTimestamp);

    _assertDocsInOplog(_opCtx.get(), {1, 2});
    auto truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);

    // Make it so that the 'recoverToStableTimestamp' method will fail.
    auto recoverToTimestampStatus =
        Status(ErrorCodes::InternalError, "recoverToStableTimestamp failed.");
    _storageInterface->setRecoverToTimestampStatus(recoverToTimestampStatus);

    // Check the current timestamp.
    ASSERT_EQUALS(currTimestamp, _storageInterface->getCurrentTimestamp());
    ASSERT_EQUALS(Timestamp(), _stableTimestamp);

    // Run rollback.
    auto rollbackStatus = _rollback->runRollback(_opCtx.get());

    // Make sure rollback failed with an UnrecoverableRollbackError, and didn't execute the
    // recover to timestamp logic.
    ASSERT_EQUALS(ErrorCodes::UnrecoverableRollbackError, rollbackStatus.code());
    ASSERT_EQUALS(currTimestamp, _storageInterface->getCurrentTimestamp());
    ASSERT_EQUALS(Timestamp(), _stableTimestamp);

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);

    // Don't set the truncate after point if we fail early.
    _assertDocsInOplog(_opCtx.get(), {1, 2});
    truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);
    ASSERT_EQUALS(_truncatePoint, Timestamp());
}

TEST_F(RollbackImplTest, RollbackReturnsBadStatusIfIncrementRollbackIDFails) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    // Delete the rollback id collection.
    auto rollbackIdNss = NamespaceString(_storageInterface->kDefaultRollbackIdNamespace);
    ASSERT_OK(_storageInterface->dropCollection(_opCtx.get(), rollbackIdNss));

    _assertDocsInOplog(_opCtx.get(), {1, 2});
    auto truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);

    // Run rollback.
    auto status = _rollback->runRollback(_opCtx.get());

    // Check that a bad status was returned since incrementing the rollback id should have
    // failed.
    ASSERT_EQUALS(ErrorCodes::NamespaceNotFound, status.code());

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);

    // Don't set the truncate after point if we fail early.
    _assertDocsInOplog(_opCtx.get(), {1, 2});
    truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);
    ASSERT_EQUALS(_truncatePoint, Timestamp());
}

TEST_F(RollbackImplTest, RollbackCallsRecoverFromOplog) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    // Run rollback.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // Make sure oplog recovery was executed.
    ASSERT(_recoveredFromOplog);
}

TEST_F(RollbackImplTest, RollbackSkipsRecoverFromOplogWhenShutdownDuringRTT) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    _assertDocsInOplog(_opCtx.get(), {1, 2});
    auto truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);

    _onRecoverToStableTimestampFn = [this](Timestamp stableTimestamp) {
        _recoveredToStableTimestamp = true;
        _stableTimestamp = stableTimestamp;
        _rollback->shutdown();
    };

    // Run rollback.
    auto status = _rollback->runRollback(_opCtx.get());

    // Make sure shutdown occurred before oplog recovery.
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _rollback->runRollback(_opCtx.get()));
    ASSERT(_recoveredToStableTimestamp);
    ASSERT_FALSE(_recoveredFromOplog);
    ASSERT_FALSE(_coordinator->lastOpTimesWereReset());

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);
    ASSERT(_stableTimestamp.isNull());

    _assertDocsInOplog(_opCtx.get(), {1, 2});
    truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);
    ASSERT_EQUALS(_truncatePoint, Timestamp());
}

TEST_F(RollbackImplTest,
       RollbackRecoversFromOplogAndFixesCountsWhenShutdownDuringSetTruncatePoint) {
    auto uuid = kGenericUUID;
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    _insertDocAndGenerateOplogEntry(BSON("_id" << 1), uuid, nss, 2);

    // Insert another document so the collection count is 2.
    const Timestamp time = Timestamp(2, 2);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss.db().toString(), uuid}, {BSON("_id" << 2), time}, time.asULL()));
    ASSERT_EQ(2ULL,
              unittest::assertGet(_storageInterface->getCollectionCount(
                  _opCtx.get(), {nss.db().toString(), uuid})));

    _assertDocsInOplog(_opCtx.get(), {1, 2});
    auto truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);
    _onSetOplogTruncateAfterPointFn = [this](Timestamp truncatePoint) {
        _truncatePoint = truncatePoint;
        _rollback->shutdown();
    };

    _onRecoverToStableTimestampFn = [this](Timestamp stableTimestamp) {
        _recoveredToStableTimestamp = true;
    };

    // Run rollback.
    auto status = _rollback->runRollback(_opCtx.get());

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _rollback->runRollback(_opCtx.get()));
    ASSERT(_recoveredToStableTimestamp);
    ASSERT(_recoveredFromOplog);
    ASSERT_FALSE(_triggeredOpObserver);

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);

    _assertDocsInOplog(_opCtx.get(), {1});
    truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);
    ASSERT_EQUALS(_truncatePoint, Timestamp(2, 2));
    ASSERT_EQ(_storageInterface->getFinalCollectionCount(uuid), 1);
}

TEST_F(RollbackImplTest, RollbackSucceedsAndTruncatesOplog) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    auto truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);
    _assertDocsInOplog(_opCtx.get(), {1, 2});

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT_EQUALS(Timestamp(1, 1), _commonPointFound);

    // Clear truncate after point after truncation.
    truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);
    _assertDocsInOplog(_opCtx.get(), {1});
    ASSERT_EQUALS(_truncatePoint, Timestamp(2, 2));
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
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

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
    ASSERT_FALSE(_coordinator->lastOpTimesWereReset());
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
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, _rollback->runRollback(_opCtx.get()));
    ASSERT(_recoveredFromOplog);
    ASSERT_FALSE(_triggeredOpObserver);
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);
}

TEST_F(RollbackImplTest, RollbackDoesNotWriteRollbackFilesIfNoInsertsOrUpdatesAfterCommonPoint) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto uuid = UUID::gen();
    const auto nss = NamespaceString("db.coll");
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    const auto oplogEntry = BSON("ts" << Timestamp(3, 3) << "h" << 3LL << "t" << 3LL << "op"
                                      << "c"
                                      << "o"
                                      << BSON("create" << nss.coll())
                                      << "ns"
                                      << nss.ns()
                                      << "ui"
                                      << uuid);
    ASSERT_OK(_insertOplogEntry(oplogEntry));

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT(_rollback->docsDeletedForNamespace_forTest(uuid).empty());
}

TEST_F(RollbackImplTest, RollbackSavesInsertedDocumentToFile) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nss = NamespaceString("db.people");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto obj = BSON("_id" << 0 << "name"
                                << "kyle");
    _insertDocAndGenerateOplogEntry(obj, uuid, nss);

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    const auto& deletedObjs = _rollback->docsDeletedForNamespace_forTest(uuid);
    ASSERT_EQ(deletedObjs.size(), 1UL);
    ASSERT_BSONOBJ_EQ(deletedObjs.front(), obj);
}

TEST_F(RollbackImplTest, RollbackSavesLatestVersionOfDocumentWhenThereAreMultipleInserts) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nss = NamespaceString("db.people");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto oldObj = BSON("_id" << 0 << "name"
                                   << "kyle");
    const auto newObj = BSON("_id" << 0 << "name"
                                   << "jungsoo");
    _insertDocAndGenerateOplogEntry(oldObj, uuid, nss);
    _deleteDocAndGenerateOplogEntry(oldObj["_id"], uuid, nss);
    _insertDocAndGenerateOplogEntry(newObj, uuid, nss);

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    const auto& deletedObjs = _rollback->docsDeletedForNamespace_forTest(uuid);
    ASSERT_EQ(deletedObjs.size(), 1UL);
    ASSERT_BSONOBJ_EQ(deletedObjs.front(), newObj);
}

TEST_F(RollbackImplTest, RollbackSavesUpdatedDocumentToFile) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nss = NamespaceString("db.people");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto query = BSON("_id" << 0);
    const auto obj = BSON("_id" << 0 << "name"
                                << "kyle");
    _updateDocAndGenerateOplogEntry(query, obj, uuid, nss);

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    const auto& deletedObjs = _rollback->docsDeletedForNamespace_forTest(uuid);
    ASSERT_EQ(deletedObjs.size(), 1UL);
    ASSERT_BSONOBJ_EQ(deletedObjs.front(), obj);
}

TEST_F(RollbackImplTest, RollbackSavesLatestVersionOfDocumentWhenThereAreMultipleUpdates) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nss = NamespaceString("db.people");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto query = BSON("_id" << 3.14);
    const auto oldObj = BSON("_id" << 3.14 << "name"
                                   << "kyle");
    const auto newObj = BSON("_id" << 3.14 << "name"
                                   << "jungsoo");
    _updateDocAndGenerateOplogEntry(query, oldObj, uuid, nss);
    _deleteDocAndGenerateOplogEntry(oldObj["_id"], uuid, nss);
    _updateDocAndGenerateOplogEntry(query, newObj, uuid, nss);

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    const auto& deletedObjs = _rollback->docsDeletedForNamespace_forTest(uuid);
    ASSERT_EQ(deletedObjs.size(), 1UL);
    ASSERT_BSONOBJ_EQ(deletedObjs.front(), newObj);
}

TEST_F(RollbackImplTest, RollbackDoesNotWriteDocumentToFileIfInsertIsRevertedByDelete) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nss = NamespaceString("db.numbers");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto objToKeep = BSON("_id" << 6);
    const auto objToDelete = BSON("_id" << 7);
    _insertDocAndGenerateOplogEntry(objToDelete, uuid, nss);
    _insertDocAndGenerateOplogEntry(objToKeep, uuid, nss);
    _deleteDocAndGenerateOplogEntry(objToDelete["_id"], uuid, nss);

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    const auto& deletedObjs = _rollback->docsDeletedForNamespace_forTest(uuid);
    ASSERT_EQ(deletedObjs.size(), 1UL);
    ASSERT_BSONOBJ_EQ(deletedObjs.front(), objToKeep);
}

TEST_F(RollbackImplTest, RollbackDoesNotWriteDocumentToFileIfUpdateIsFollowedByDelete) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nss = NamespaceString("db.numbers");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto objToKeep = BSON("_id" << 6);
    const auto objToDelete = BSON("_id" << 7);
    _updateDocAndGenerateOplogEntry(objToDelete, objToDelete, uuid, nss);
    _updateDocAndGenerateOplogEntry(objToKeep, objToKeep, uuid, nss);
    _deleteDocAndGenerateOplogEntry(objToDelete["_id"], uuid, nss);

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    const auto& deletedObjs = _rollback->docsDeletedForNamespace_forTest(uuid);
    ASSERT_EQ(deletedObjs.size(), 1UL);
    ASSERT_BSONOBJ_EQ(deletedObjs.front(), objToKeep);
}

TEST_F(RollbackImplTest, RollbackProperlySavesFilesWhenCollectionIsRenamed) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nssBeforeRename = NamespaceString("db.firstColl");
    const auto uuidBeforeRename = UUID::gen();
    const auto collBeforeRename =
        _initializeCollection(_opCtx.get(), uuidBeforeRename, nssBeforeRename);

    // Insert a document into the collection.
    const auto objInRenamedCollection = BSON("_id"
                                             << "kyle");
    _insertDocAndGenerateOplogEntry(objInRenamedCollection, uuidBeforeRename, nssBeforeRename, 2);

    // Rename the original collection.
    const auto nssAfterRename = NamespaceString("db.secondColl");
    auto renameCmdObj =
        BSON("renameCollection" << nssBeforeRename.ns() << "to" << nssAfterRename.ns());
    auto renameCmdOp =
        makeCommandOp(Timestamp(3, 3), uuidBeforeRename, nssBeforeRename.ns(), renameCmdObj, 3);
    ASSERT_OK(_insertOplogEntry(renameCmdOp.first));
    ASSERT_OK(
        _storageInterface->renameCollection(_opCtx.get(), nssBeforeRename, nssAfterRename, true));

    // Create a new collection with the old name.
    const auto uuidAfterRename = UUID::gen();
    const auto collAfterRename =
        _initializeCollection(_opCtx.get(), uuidAfterRename, nssBeforeRename);

    // Insert a different document into the new collection.
    const auto objInNewCollection = BSON("_id"
                                         << "jungsoo");
    _insertDocAndGenerateOplogEntry(objInNewCollection, uuidAfterRename, nssBeforeRename, 4);

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    const auto& deletedObjsRenamedColl =
        _rollback->docsDeletedForNamespace_forTest(uuidBeforeRename);
    ASSERT_EQ(deletedObjsRenamedColl.size(), 1UL);
    ASSERT_BSONOBJ_EQ(deletedObjsRenamedColl.front(), objInRenamedCollection);

    const auto& deletedObjsNewColl = _rollback->docsDeletedForNamespace_forTest(uuidAfterRename);
    ASSERT_EQ(deletedObjsNewColl.size(), 1UL);
    ASSERT_BSONOBJ_EQ(deletedObjsNewColl.front(), objInNewCollection);
}

TEST_F(RollbackImplTest, RollbackProperlySavesFilesWhenInsertsAndDropOfCollectionAreRolledBack) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    // Create the collection, but as a drop-pending collection.
    const auto dropOpTime = OpTime(Timestamp(200, 200), 200L);
    const auto nss = NamespaceString("db.people").makeDropPendingNamespace(dropOpTime);
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    DropPendingCollectionReaper::get(_opCtx.get())->addDropPendingNamespace(dropOpTime, nss);

    // Insert documents into the collection. We'll write them out even though the collection is
    // later dropped.
    const auto obj1 = BSON("_id"
                           << "kyle");
    const auto obj2 = BSON("_id"
                           << "glenn");
    _insertDocAndGenerateOplogEntry(obj1, uuid, nss);
    _insertDocAndGenerateOplogEntry(obj2, uuid, nss);

    // Create an oplog entry for the collection drop.
    const auto oplogEntry = BSON(
        "ts" << dropOpTime.getTimestamp() << "h" << 200LL << "t" << dropOpTime.getTerm() << "op"
             << "c"
             << "o"
             << BSON("drop" << nss.coll())
             << "ns"
             << nss.ns()
             << "ui"
             << uuid);
    ASSERT_OK(_insertOplogEntry(oplogEntry));

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    const auto& deletedObjs = _rollback->docsDeletedForNamespace_forTest(uuid);
    ASSERT_EQ(deletedObjs.size(), 2UL);
    std::vector<BSONObj> expectedObjs({obj1, obj2});
    ASSERT(std::is_permutation(deletedObjs.begin(),
                               deletedObjs.end(),
                               expectedObjs.begin(),
                               expectedObjs.end(),
                               SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(RollbackImplTest, RollbackProperlySavesFilesWhenCreateCollAndInsertsAreRolledBack) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    // Create the collection and make an oplog entry for the creation event.
    const auto nss = NamespaceString("db.people");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    const auto oplogEntry = BSON("ts" << Timestamp(3, 3) << "h" << 3LL << "t" << 3LL << "op"
                                      << "c"
                                      << "o"
                                      << BSON("create" << nss.coll())
                                      << "ns"
                                      << nss.ns()
                                      << "ui"
                                      << uuid);
    ASSERT_OK(_insertOplogEntry(oplogEntry));

    // Insert documents into the collection.
    const std::vector<BSONObj> objs({BSON("_id"
                                          << "kyle"),
                                     BSON("_id"
                                          << "jungsoo"),
                                     BSON("_id"
                                          << "erjon")});
    for (auto&& obj : objs) {
        _insertDocAndGenerateOplogEntry(obj, uuid, nss);
    }

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    const auto& deletedObjs = _rollback->docsDeletedForNamespace_forTest(uuid);
    ASSERT_EQ(deletedObjs.size(), objs.size());
    ASSERT(std::is_permutation(deletedObjs.begin(),
                               deletedObjs.end(),
                               objs.begin(),
                               objs.end(),
                               SimpleBSONObjComparator::kInstance.makeEqualTo()));
}

TEST_F(RollbackImplTest, RollbackStopsWritingRollbackFilesWhenShutdownIsInProgress) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nss1 = NamespaceString("db.people");
    const auto uuid1 = UUID::gen();
    const auto coll1 = _initializeCollection(_opCtx.get(), uuid1, nss1);
    const auto obj1 = BSON("_id" << 0 << "name"
                                 << "kyle");
    _insertDocAndGenerateOplogEntry(obj1, uuid1, nss1);

    const auto nss2 = NamespaceString("db.persons");
    const auto uuid2 = UUID::gen();
    const auto coll2 = _initializeCollection(_opCtx.get(), uuid2, nss2);
    const auto obj2 = BSON("_id" << 0 << "name"
                                 << "jungsoo");
    _insertDocAndGenerateOplogEntry(obj2, uuid2, nss2);

    // Register a listener that sends rollback into shutdown.
    std::vector<UUID> collsWithSuccessfullyWrittenDataFiles;
    _onRollbackFileWrittenForNamespaceFn =
        [this, &collsWithSuccessfullyWrittenDataFiles](UUID uuid, NamespaceString nss) {
            collsWithSuccessfullyWrittenDataFiles.emplace_back(std::move(uuid));
            _rollback->shutdown();
        };

    ASSERT_EQ(_rollback->runRollback(_opCtx.get()), ErrorCodes::ShutdownInProgress);

    ASSERT_EQ(collsWithSuccessfullyWrittenDataFiles.size(), 1UL);
    const auto& uuid = collsWithSuccessfullyWrittenDataFiles.front();
    ASSERT(uuid == uuid1 || uuid == uuid2) << "wrote out a data file for unknown uuid " << uuid
                                           << "; expected it to be either " << uuid1 << " or "
                                           << uuid2;
}

DEATH_TEST_F(RollbackImplTest,
             InvariantFailureIfNamespaceIsMissingWhenWritingRollbackFiles,
             "unexpectedly missing in the UUIDCatalog") {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nss = NamespaceString("db.people");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto obj = BSON("_id" << 0 << "name"
                                << "kyle");
    _insertDocAndGenerateOplogEntry(obj, uuid, nss);

    // Drop the collection (immediately; not a two-phase drop), so that the namespace can no longer
    // be found.
    ASSERT_OK(_storageInterface->dropCollection(_opCtx.get(), nss));

    auto status = _rollback->runRollback(_opCtx.get());
    unittest::log() << "mongod did not crash when expected; status: " << status;
}

DEATH_TEST_F(RollbackImplTest,
             InvariantFailureIfNamespaceIsMissingWhenGettingCollectionSizes,
             "unexpectedly missing in the UUIDCatalog") {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    ASSERT_OK(_insertOplogEntry(makeDeleteOplogEntry(2, BSON("_id" << 1), nss.ns(), kGenericUUID)));

    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    auto status = _rollback->runRollback(_opCtx.get());
    unittest::log() << "mongod did not crash when expected; status: " << status;
}

TEST_F(RollbackImplTest, RollbackSetsMultipleCollectionCounts) {
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));

    auto uuid1 = UUID::gen();
    auto nss1 = NamespaceString("test.coll1");
    const auto obj1 = BSON("_id" << 1);
    const auto coll1 = _initializeCollection(_opCtx.get(), uuid1, nss1);
    _insertDocAndGenerateOplogEntry(obj1, uuid1, nss1, 2);

    const Timestamp time1 = Timestamp(2, 2);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss1.db().toString(), uuid1}, {BSON("_id" << 2), time1}, time1.asULL()));
    ASSERT_EQ(2ULL,
              unittest::assertGet(_storageInterface->getCollectionCount(
                  _opCtx.get(), {nss1.db().toString(), uuid1})));
    ASSERT_OK(
        _storageInterface->setCollectionCount(_opCtx.get(), {nss1.db().toString(), uuid1}, 2));

    auto uuid2 = UUID::gen();
    auto nss2 = NamespaceString("test.coll2");
    const auto obj2 = BSON("_id" << 1);
    const auto coll2 = _initializeCollection(_opCtx.get(), uuid2, nss2);
    const Timestamp time2 = Timestamp(3, 3);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss2.db().toString(), uuid2}, {obj2, time2}, time2.asULL()));
    _deleteDocAndGenerateOplogEntry(obj2["_id"], uuid2, nss2, 3);

    const Timestamp time3 = Timestamp(4, 4);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss2.db().toString(), uuid2}, {BSON("_id" << 2), time3}, time3.asULL()));
    ASSERT_EQ(1ULL,
              unittest::assertGet(_storageInterface->getCollectionCount(
                  _opCtx.get(), {nss2.db().toString(), uuid2})));
    ASSERT_OK(
        _storageInterface->setCollectionCount(_opCtx.get(), {nss2.db().toString(), uuid2}, 1));

    _assertDocsInOplog(_opCtx.get(), {1, 2, 3});

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT_EQ(_storageInterface->getFinalCollectionCount(uuid1), 1);
    ASSERT_EQ(_storageInterface->getFinalCollectionCount(uuid2), 2);
}

TEST_F(RollbackImplTest, CountChangesCancelOut) {
    auto uuid = kGenericUUID;
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));

    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    const auto obj = BSON("_id" << 2);
    const Timestamp time = Timestamp(2, 2);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss.db().toString(), uuid}, {obj, time}, time.asULL()));

    _insertDocAndGenerateOplogEntry(BSON("_id" << 1), uuid, nss, 2);
    _deleteDocAndGenerateOplogEntry(obj["_id"], uuid, nss, 3);
    _insertDocAndGenerateOplogEntry(BSON("_id" << 3), uuid, nss, 4);

    // Test that we do nothing on drop oplog entries.
    ASSERT_OK(_insertOplogEntry(makeCommandOp(Timestamp(5, 5),
                                              UUID::gen(),
                                              nss.getCommandNS().toString(),
                                              BSON("drop" << nss.coll()),
                                              5)
                                    .first));

    ASSERT_EQ(2ULL,
              unittest::assertGet(_storageInterface->getCollectionCount(
                  _opCtx.get(), {nss.db().toString(), uuid})));
    ASSERT_OK(_storageInterface->setCollectionCount(nullptr, {"", uuid}, 2));

    _assertDocsInOplog(_opCtx.get(), {1, 2, 3, 4, 5});

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT_EQ(_storageInterface->getFinalCollectionCount(uuid), 1);
}

TEST_F(RollbackImplTest, RollbackIgnoresSetCollectionCountError) {
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));

    auto uuid1 = UUID::gen();
    auto nss1 = NamespaceString("test.coll1");
    const auto obj1 = BSON("_id" << 1);
    const auto coll1 = _initializeCollection(_opCtx.get(), uuid1, nss1);
    _insertDocAndGenerateOplogEntry(obj1, uuid1, nss1, 2);

    const Timestamp time1 = Timestamp(2, 2);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss1.db().toString(), uuid1}, {BSON("_id" << 2), time1}, time1.asULL()));
    ASSERT_EQ(2ULL,
              unittest::assertGet(_storageInterface->getCollectionCount(
                  _opCtx.get(), {nss1.db().toString(), uuid1})));
    ASSERT_OK(_storageInterface->setCollectionCount(nullptr, {"", uuid1}, 2));

    auto uuid2 = UUID::gen();
    auto nss2 = NamespaceString("test.coll2");
    const auto obj2 = BSON("_id" << 1);
    const auto coll2 = _initializeCollection(_opCtx.get(), uuid2, nss2);
    _insertDocAndGenerateOplogEntry(obj2, uuid2, nss2, 3);

    const Timestamp time2 = Timestamp(3, 3);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss2.db().toString(), uuid2}, {BSON("_id" << 2), time2}, time2.asULL()));
    ASSERT_EQ(2ULL,
              unittest::assertGet(_storageInterface->getCollectionCount(
                  _opCtx.get(), {nss2.db().toString(), uuid2})));
    ASSERT_OK(_storageInterface->setCollectionCount(nullptr, {"", uuid2}, 2));

    _assertDocsInOplog(_opCtx.get(), {1, 2, 3});

    _storageInterface->setSetCollectionCountStatus(
        uuid1, Status(ErrorCodes::CommandFailed, "set collection count failed"));

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT_EQ(_storageInterface->getFinalCollectionCount(uuid1), 2);
    ASSERT_EQ(_storageInterface->getFinalCollectionCount(uuid2), 1);
}

TEST_F(RollbackImplTest, ResetToZeroIfCountGoesNegative) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    ASSERT_OK(_insertOplogEntry(makeInsertOplogEntry(2, BSON("_id" << 1), nss.ns(), kGenericUUID)));

    const auto coll = _initializeCollection(_opCtx.get(), kGenericUUID, nss);

    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT_EQ(_storageInterface->getFinalCollectionCount(kGenericUUID), 0);
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
        _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

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

    BSONObj makeSessionOp(UUID collId, NamespaceString nss, UUID sessionId, TxnNumber txnNum) {
        auto doc = BSON("_id" << 1);
        const Timestamp time = Timestamp(2, 1);
        ASSERT_OK(_storageInterface->insertDocument(
            _opCtx.get(), {nss.db().toString(), collId}, {doc, time}, time.asULL()));

        BSONObjBuilder bob;
        bob.append("ts", time);
        bob.append("h", 1LL);
        bob.append("op", "i");
        collId.appendToBuilder(&bob, "ui");
        bob.append("ns", nss.ns());
        bob.append("o", doc);
        bob.append("lsid",
                   BSON("id" << sessionId << "uid"
                             << BSONBinData(std::string(32, 'x').data(), 32, BinDataGeneral)));
        bob.append("txnNumber", txnNum);
        return bob.obj();
    }

    void assertRollbackInfoContainsObjectForUUID(UUID uuid, BSONObj bson) {
        const auto& uuidToIdMap = _rbInfo.rollbackDeletedIdsMap;
        auto search = uuidToIdMap.find(uuid);
        ASSERT(search != uuidToIdMap.end()) << "map is unexpectedly missing an entry for uuid "
                                            << uuid.toString() << " containing object "
                                            << bson.jsonString();
        const auto& idObjSet = search->second;
        const auto iter = idObjSet.find(bson);
        ASSERT(iter != idObjSet.end()) << "_id object set is unexpectedly missing object "
                                       << bson.jsonString() << " in namespace with uuid "
                                       << uuid.toString();
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
    const auto nss = NamespaceString("test", "coll");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    const auto insertOp = _insertDocAndReturnOplogEntry(BSON("_id" << 1), uuid, nss, 2);

    ASSERT_OK(rollbackOps({insertOp}));
    std::set<NamespaceString> expectedNamespaces = {nss};
    ASSERT(expectedNamespaces == _rbInfo.rollbackNamespaces);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsMultipleNamespacesOfOplogEntries) {
    const auto makeInsertOp = [&](UUID uuid, NamespaceString nss, Timestamp ts, int recordId) {
        return _insertDocAndReturnOplogEntry(BSON("_id" << 1), uuid, nss, recordId);
    };

    const auto nss1 = NamespaceString("test", "coll1");
    const auto nss2 = NamespaceString("test", "coll2");
    const auto nss3 = NamespaceString("test", "coll3");

    const auto uuid1 = UUID::gen();
    const auto uuid2 = UUID::gen();
    const auto uuid3 = UUID::gen();

    const auto coll1 = _initializeCollection(_opCtx.get(), uuid1, nss1);
    const auto coll2 = _initializeCollection(_opCtx.get(), uuid2, nss2);
    const auto coll3 = _initializeCollection(_opCtx.get(), uuid3, nss3);

    auto insertOp1 = makeInsertOp(uuid1, nss1, Timestamp(2, 1), 2);
    auto insertOp2 = makeInsertOp(uuid2, nss2, Timestamp(3, 1), 3);
    auto insertOp3 = makeInsertOp(uuid3, nss3, Timestamp(4, 1), 4);

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
    const auto collId = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), collId, nss);
    auto sessionId = UUID::gen();
    auto sessionOpObj = makeSessionOp(collId, nss, sessionId, 1L);
    auto sessionOp = std::make_pair(sessionOpObj, RecordId(recordId));

    // Run the rollback and make sure the correct session id was recorded.
    ASSERT_OK(rollbackOps({sessionOp}));
    std::set<UUID> expectedSessionIds = {sessionId};
    ASSERT(expectedSessionIds == _rbInfo.rollbackSessionIds);
}

TEST_F(RollbackImplObserverInfoTest,
       RollbackDoesntRecordSessionIdFromOplogEntryWithoutSessionInfo) {
    const auto nss = NamespaceString("test", "coll");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto insertOp = _insertDocAndReturnOplogEntry(BSON("_id" << 1), uuid, nss, 2);
    ASSERT_OK(rollbackOps({insertOp}));
    ASSERT(_rbInfo.rollbackSessionIds.empty());
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsSessionIdFromApplyOpsSubOp) {
    const auto collId = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), collId, nss);
    auto sessionId = UUID::gen();
    auto sessionOpObj = makeSessionOp(collId, nss, sessionId, 1L);

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
    const auto uuid = UUID::gen();
    const auto nss = NamespaceString::kServerConfigurationNamespace;
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto insertShardIdOp =
        _insertDocAndReturnOplogEntry(BSON("_id" << ShardIdentityType::IdName), uuid, nss, 2);
    ASSERT_OK(rollbackOps({insertShardIdOp}));
    ASSERT(_rbInfo.shardIdentityRolledBack);
}

TEST_F(RollbackImplObserverInfoTest, RollbackDoesntRecordShardIdentityRollbackForNormalDocument) {
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    const auto nss = NamespaceString::kServerConfigurationNamespace;
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    auto deleteOp = makeCRUDOp(OpTypeEnum::kDelete,
                               Timestamp(2, 2),
                               uuid,
                               nss.ns(),
                               BSON("_id"
                                    << "not_the_shard_id_document"),
                               boost::none,
                               2);
    ASSERT_OK(rollbackOps({deleteOp}));
    ASSERT_FALSE(_rbInfo.shardIdentityRolledBack);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsConfigVersionRollback) {
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    const auto uuid = UUID::gen();
    const auto nss = VersionType::ConfigNS;
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    auto insertOp = makeCRUDOp(OpTypeEnum::kInsert,
                               Timestamp(2, 2),
                               uuid,
                               nss.ns(),
                               BSON("_id"
                                    << "a"),
                               boost::none,
                               2);

    ASSERT_OK(rollbackOps({insertOp}));
    ASSERT(_rbInfo.configServerConfigVersionRolledBack);
}

TEST_F(RollbackImplObserverInfoTest, RollbackDoesntRecordConfigVersionRollbackForShardServer) {
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    const auto uuid = UUID::gen();
    const auto nss = VersionType::ConfigNS;
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    auto insertOp = makeCRUDOp(OpTypeEnum::kInsert,
                               Timestamp(2, 2),
                               uuid,
                               nss.ns(),
                               BSON("_id"
                                    << "a"),
                               boost::none,
                               2);
    ASSERT_OK(rollbackOps({insertOp}));
    ASSERT_FALSE(_rbInfo.configServerConfigVersionRolledBack);
}

TEST_F(RollbackImplObserverInfoTest, RollbackDoesntRecordConfigVersionRollbackForNonInsert) {
    serverGlobalParams.clusterRole = ClusterRole::ConfigServer;
    const auto uuid = UUID::gen();
    const auto nss = VersionType::ConfigNS;
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    auto deleteOp = makeCRUDOp(OpTypeEnum::kDelete,
                               Timestamp(2, 2),
                               uuid,
                               nss.ns(),
                               BSON("_id"
                                    << "a"),
                               boost::none,
                               2);
    ASSERT_OK(rollbackOps({deleteOp}));
    ASSERT_FALSE(_rbInfo.configServerConfigVersionRolledBack);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsInsertOpsInUUIDToIdMap) {
    const auto nss1 = NamespaceString("db.people");
    const auto uuid1 = UUID::gen();
    const auto coll1 = _initializeCollection(_opCtx.get(), uuid1, nss1);
    const auto obj1 = BSON("_id"
                           << "kyle");
    const auto insertOp1 = _insertDocAndReturnOplogEntry(obj1, uuid1, nss1, 2);

    const auto nss2 = NamespaceString("db.persons");
    const auto uuid2 = UUID::gen();
    const auto coll2 = _initializeCollection(_opCtx.get(), uuid2, nss2);
    const auto obj2 = BSON("_id"
                           << "jungsoo");
    const auto insertOp2 = _insertDocAndReturnOplogEntry(obj2, uuid2, nss2, 3);

    ASSERT_OK(rollbackOps({insertOp2, insertOp1}));
    ASSERT_EQ(_rbInfo.rollbackDeletedIdsMap.size(), 2UL);

    assertRollbackInfoContainsObjectForUUID(uuid1, obj1);
    assertRollbackInfoContainsObjectForUUID(uuid2, obj2);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsUpdateOpsInUUIDToIdMap) {
    const auto nss1 = NamespaceString("db.coll1");
    const auto uuid1 = UUID::gen();
    const auto coll1 = _initializeCollection(_opCtx.get(), uuid1, nss1);
    const auto id1 = BSON("_id" << 1);
    const auto updateOp1 = makeCRUDOp(OpTypeEnum::kUpdate,
                                      Timestamp(2, 2),
                                      uuid1,
                                      nss1.ns(),
                                      BSON("$set" << BSON("foo" << 1)),
                                      id1,
                                      2);

    const auto nss2 = NamespaceString("db.coll2");
    const auto uuid2 = UUID::gen();
    const auto coll2 = _initializeCollection(_opCtx.get(), uuid2, nss2);
    const auto id2 = BSON("_id" << 2);
    const auto updateOp2 = makeCRUDOp(OpTypeEnum::kUpdate,
                                      Timestamp(3, 3),
                                      uuid2,
                                      nss2.ns(),
                                      BSON("$set" << BSON("foo" << 1)),
                                      id2,
                                      3);

    ASSERT_OK(rollbackOps({updateOp2, updateOp1}));
    ASSERT_EQ(_rbInfo.rollbackDeletedIdsMap.size(), 2UL);

    assertRollbackInfoContainsObjectForUUID(uuid1, id1);
    assertRollbackInfoContainsObjectForUUID(uuid2, id2);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsMultipleInsertOpsForSameNamespace) {
    const auto nss = NamespaceString("db.coll");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto obj1 = BSON("_id" << 1);
    const auto insertOp1 = _insertDocAndReturnOplogEntry(obj1, uuid, nss, 2);

    const auto obj2 = BSON("_id" << 2);
    const auto insertOp2 = _insertDocAndReturnOplogEntry(obj2, uuid, nss, 3);

    ASSERT_OK(rollbackOps({insertOp2, insertOp1}));
    ASSERT_EQ(_rbInfo.rollbackDeletedIdsMap.size(), 1UL);

    assertRollbackInfoContainsObjectForUUID(uuid, obj1);
    assertRollbackInfoContainsObjectForUUID(uuid, obj2);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsMultipleUpdateOpsForSameNamespace) {
    const auto nss = NamespaceString("db.coll");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    const auto obj1 = BSON("_id" << 1);
    const auto updateOp1 = makeCRUDOp(OpTypeEnum::kUpdate,
                                      Timestamp(2, 2),
                                      uuid,
                                      nss.ns(),
                                      BSON("$set" << BSON("foo" << 1)),
                                      obj1,
                                      2);

    const auto obj2 = BSON("_id" << 2);
    const auto updateOp2 = makeCRUDOp(OpTypeEnum::kUpdate,
                                      Timestamp(3, 3),
                                      uuid,
                                      nss.ns(),
                                      BSON("$set" << BSON("bar" << 2)),
                                      obj2,
                                      3);

    ASSERT_OK(rollbackOps({updateOp2, updateOp1}));
    ASSERT_EQ(_rbInfo.rollbackDeletedIdsMap.size(), 1UL);

    assertRollbackInfoContainsObjectForUUID(uuid, obj1);
    assertRollbackInfoContainsObjectForUUID(uuid, obj2);
}
}  // namespace
