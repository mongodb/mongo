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

#include "mongo/db/repl/rollback_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/oid.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/global_catalog/type_shard_identity.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/lock_manager/lock_manager_defs.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/op_observer/op_observer_util.h"
#include "mongo/db/record_id.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_interface_local.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/replication_consistency_markers.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/rollback_test_fixture.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/storage/durable_history_pin.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <ostream>
#include <utility>
#include <vector>

#include <boost/cstdint.hpp>
#include <boost/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kReplicationRollback

namespace mongo {
namespace repl {
namespace {

NamespaceString kOplogNSS = NamespaceString::createNamespaceString_forTest("local.oplog.rs");
NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.coll");
std::string kGenericUUIDStr = "b4c66a44-c1ca-4d86-8d25-12e82fa2de5b";

BSONObj makeInsertOplogEntry(long long time, BSONObj obj, StringData ns, UUID uuid) {
    return BSON("ts" << Timestamp(time, time) << "t" << time << "op"
                     << "i"
                     << "o" << obj << "ns" << ns << "ui" << uuid << "wall" << Date_t());
}

BSONObj makeUpdateOplogEntry(
    long long time, BSONObj query, BSONObj update, StringData ns, UUID uuid) {
    return BSON("ts" << Timestamp(time, time) << "t" << time << "op"
                     << "u"
                     << "ns" << ns << "ui" << uuid << "o2" << query << "o" << BSON("$set" << update)
                     << "wall" << Date_t());
}

BSONObj makeDeleteOplogEntry(long long time, BSONObj id, StringData ns, UUID uuid) {
    return BSON("ts" << Timestamp(time, time) << "t" << time << "op"
                     << "d"
                     << "ns" << ns << "ui" << uuid << "o" << id << "wall" << Date_t());
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
        LOGV2(21647,
              "Simulating writing a rollback file for namespace {nss_ns} with uuid {uuid}",
              logAttrs(nss),
              "uuid"_attr = uuid);
        for (auto&& id : idSet) {
            LOGV2(21648,
                  "Looking up {id_jsonString_JsonStringFormat_LegacyStrict}",
                  "id_jsonString_JsonStringFormat_LegacyStrict"_attr =
                      id.jsonString(JsonStringFormat::LegacyStrict));
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
     * with the new collection in the CollectionCatalog. There must not already exist a collection
     * with
     * name 'nss'.
     */
    std::unique_ptr<Collection> _initializeCollection(OperationContext* opCtx,
                                                      UUID uuid,
                                                      const NamespaceString& nss) {
        // Create a new collection in the storage interface.
        CollectionOptions options;
        options.uuid = uuid;
        OperationShardingState::ScopedAllowImplicitCollectionCreate_UNSAFE unsafeCreateCollection(
            opCtx, nss);
        ASSERT_OK(_storageInterface->createCollection(opCtx, nss, options));

        // Initialize a mock collection.
        return std::make_unique<CollectionMock>(nss);
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
        ASSERT_OK(_insertOplogEntry(makeInsertOplogEntry(optime, doc, nss.ns_forTest(), uuid)));
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
        return std::make_pair(makeInsertOplogEntry(optime, doc, nss.ns_forTest(), uuid),
                              RecordId(optime));
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
        ASSERT_OK(
            _insertOplogEntry(makeUpdateOplogEntry(time, query, newDoc, nss.ns_forTest(), uuid)));
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
        ASSERT_OK(_insertOplogEntry(makeDeleteOplogEntry(time, id.wrap(), nss.ns_forTest(), uuid)));
        WriteUnitOfWork wuow{_opCtx.get()};
        ASSERT_OK(_storageInterface->deleteById(_opCtx.get(), nss, id));
        ASSERT_OK(
            shard_role_details::getRecoveryUnit(_opCtx.get())->setTimestamp(Timestamp(time, time)));
        wuow.commit();
    }

    /**
     * Sets up a test for an unprepared transaction that is comprised of multiple
     * applyOps oplog entries.
     * Returns entries appended to the oplog.
     */
    std::vector<OplogInterfaceMock::Operation> _setUpUnpreparedTransactionForCountTest(UUID collId);

    /**
     * Sets up a test for a retryable write that is comprised of multiple applyOps oplog entries.
     *
     * Returns entries appended to the oplog.
     */
    std::vector<OplogInterfaceMock::Operation> _setUpBatchedRetryableWriteForCountTest(UUID collId);

    std::unique_ptr<OplogInterfaceLocal> _localOplog;
    std::unique_ptr<OplogInterfaceMock> _remoteOplog;
    std::unique_ptr<RollbackImplForTest> _rollback;

    bool _transitionedToRollback = false;
    std::function<void()> _onTransitionToRollbackFn = [this]() {
        _transitionedToRollback = true;
    };

    bool _recoveredToStableTimestamp = false;
    Timestamp _stableTimestamp;
    std::function<void(Timestamp)> _onRecoverToStableTimestampFn =
        [this](Timestamp stableTimestamp) {
            _recoveredToStableTimestamp = true;
            _stableTimestamp = stableTimestamp;
        };

    bool _recoveredFromOplog = false;
    std::function<void()> _onRecoverFromOplogFn = [this]() {
        _recoveredFromOplog = true;
    };

    bool _incrementedRollbackID = false;
    std::function<void()> _onRollbackIDIncrementedFn = [this]() {
        _incrementedRollbackID = true;
    };

    bool _reconstructedPreparedTransactions = false;
    std::function<void()> _onPreparedTransactionsReconstructedFn = [this]() {
        _reconstructedPreparedTransactions = true;
    };

    Timestamp _commonPointFound;
    std::function<void(Timestamp commonPoint)> _onCommonPointFoundFn =
        [this](Timestamp commonPoint) {
            _commonPointFound = commonPoint;
        };

    Timestamp _truncatePoint;
    std::function<void(Timestamp truncatePoint)> _onSetOplogTruncateAfterPointFn =
        [this](Timestamp truncatePoint) {
            _truncatePoint = truncatePoint;
        };

    bool _triggeredOpObserver = false;
    std::function<void(const OpObserver::RollbackObserverInfo& rbInfo)> _onRollbackOpObserverFn =
        [this](const OpObserver::RollbackObserverInfo& rbInfo) {
            _triggeredOpObserver = true;
        };

    std::function<void(UUID, NamespaceString)> _onRollbackFileWrittenForNamespaceFn =
        [this](UUID, NamespaceString) {
        };

    std::unique_ptr<Listener> _listener;

    UUID kGenericUUID = unittest::assertGet(UUID::parse(kGenericUUIDStr));

    const long kOneDayInMillis = 60 * 60 * 24 * 1000;

private:
    long _counter = 100;
};

void RollbackImplTest::setUp() {
    RollbackTest::setUp();

    _localOplog = std::make_unique<OplogInterfaceLocal>(_opCtx.get());
    _remoteOplog = std::make_unique<OplogInterfaceMock>();
    _listener = std::make_unique<Listener>(this);
    _rollback = std::make_unique<RollbackImplForTest>(_localOplog.get(),
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

    void onRollbackIDIncremented() noexcept override {
        _test->_onRollbackIDIncrementedFn();
    }

    void onRollbackFileWrittenForNamespace(UUID uuid, NamespaceString nss) noexcept final {
        _test->_onRollbackFileWrittenForNamespaceFn(std::move(uuid), std::move(nss));
    }

    void onRecoverToStableTimestamp(Timestamp stableTimestamp) override {
        _test->_onRecoverToStableTimestampFn(stableTimestamp);
    }

    void onSetOplogTruncateAfterPoint(Timestamp truncatePoint) noexcept override {
        _test->_onSetOplogTruncateAfterPointFn(truncatePoint);
    }

    void onRecoverFromOplog() noexcept override {
        _test->_onRecoverFromOplogFn();
    }

    void onPreparedTransactionsReconstructed() noexcept override {
        _test->_onPreparedTransactionsReconstructedFn();
    }

    void onRollbackOpObserver(const OpObserver::RollbackObserverInfo& rbInfo) noexcept override {
        _test->_onRollbackOpObserverFn(rbInfo);
    }

private:
    RollbackImplTest* _test;
};

/**
 * Helper functions to make simple oplog entries with timestamps and terms.
 */
BSONObj makeOp(OpTime time) {
    auto kGenericUUID = unittest::assertGet(UUID::parse(kGenericUUIDStr));
    return BSON("ts" << time.getTimestamp() << "t" << time.getTerm() << "op"
                     << "n"
                     << "o" << BSONObj() << "ns" << nss.ns_forTest() << "ui" << kGenericUUID
                     << "wall" << Date_t());
}

BSONObj makeOp(int count) {
    return makeOp(OpTime(Timestamp(count, count), count));
}

/**
 * Helper functions to make simple oplog entries with timestamps, terms, and wall clock
 * times.
 */
auto makeOpWithWallClockTime(long count, long wallClockMillis) {
    auto kGenericUUID = unittest::assertGet(UUID::parse(kGenericUUIDStr));
    return BSON("ts" << Timestamp(count, count) << "t" << (long long)count << "op"
                     << "n"
                     << "o" << BSONObj() << "ns"
                     << "top"
                     << "ui" << kGenericUUID << "wall"
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

OplogInterfaceMock::Operation makeOpAndRecordId(OpTime time) {
    return makeOpAndRecordId(makeOp(time));
}

OplogInterfaceMock::Operation makeOpAndRecordId(int count) {
    return makeOpAndRecordId(makeOp(count));
}

/**
 * Helper to create a noop entry that represents a migrated retryable write or transaction oplog
 * entry.
 */
BSONObj makeMigratedNoop(OpTime opTime,
                         boost::optional<BSONObj> o2,
                         LogicalSessionId lsid,
                         int txnNum,
                         OpTime prevOpTime,
                         boost::optional<int> stmtId,
                         int wallClockMillis,
                         bool isRetryableWrite) {
    repl::MutableOplogEntry op;
    op.setOpType(repl::OpTypeEnum::kNoop);
    op.setNss(nss);
    op.setObject(BSONObj());
    op.setOpTime(opTime);
    if (isRetryableWrite) {
        op.setFromMigrate(true);
    }
    op.setObject2(o2);
    if (stmtId) {
        op.setStatementIds({*stmtId});
    }
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNum);
    op.setOperationSessionInfo(sessionInfo);
    op.setPrevWriteOpTimeInTransaction(prevOpTime);
    op.setWallClockTime(Date_t::fromMillisSinceEpoch(wallClockMillis));
    return op.toBSON();
}

/**
 * Helper to create a transaction command oplog entry.
 */
BSONObj makeTransactionOplogEntry(OpTime opTime,
                                  LogicalSessionId lsid,
                                  int txnNum,
                                  OpTime prevOpTime) {
    repl::MutableOplogEntry op;
    op.setOpType(repl::OpTypeEnum::kCommand);
    op.setNss(nss);
    op.setObject(BSON("applyOps" << BSONArray()));
    op.setOpTime(opTime);
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNum);
    op.setOperationSessionInfo(sessionInfo);
    op.setPrevWriteOpTimeInTransaction(prevOpTime);
    op.setWallClockTime(Date_t());
    return op.toBSON();
}

/**
 * Asserts that the documents in the oplog have the given timestamps.
 */
void _assertDocsInOplog(OperationContext* opCtx, std::vector<int> timestamps) {
    std::vector<BSONObj> expectedOplog(timestamps.size());
    std::transform(timestamps.begin(), timestamps.end(), expectedOplog.begin(), [](int ts) {
        return makeOp(ts);
    });

    OplogInterfaceLocal oplog(opCtx);
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
    auto serviceContext = getServiceContext();
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
    auto commonPoint = makeOpAndRecordId(makeOpWithWallClockTime(1, 5 * 1000));
    // We use the difference of wall clock times between the top of the oplog and the first op after
    // the common point to calculate the rollback time limit.
    auto firstOpAfterCommonPoint =
        makeOpAndRecordId(makeOpWithWallClockTime(2, 2 * kOneDayInMillis));
    // The default limit is 1 day, so we make the difference be just under a day.
    auto topOfOplog = makeOpAndRecordId(makeOpWithWallClockTime(3, kOneDayInMillis));
    _remoteOplog->setOperations({commonPoint});
    ASSERT_OK(_insertOplogEntry(commonPoint.first));
    ASSERT_OK(_insertOplogEntry(firstOpAfterCommonPoint.first));
    ASSERT_OK(_insertOplogEntry(topOfOplog.first));

    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    // Run rollback.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
}

TEST_F(RollbackImplTest, RollbackSucceedsIfTopOfOplogIsFirstOpAfterCommonPoint) {

    auto commonPoint = makeOpAndRecordId(makeOpWithWallClockTime(1, 5 * 1000));
    // The default limit is 1 day, so we make the difference be 2 days. The rollback should still
    // succeed since we calculate the difference of wall clock times between the top of the oplog
    // and the first op after the common point which are both the same operation in this case.
    auto topOfOplog = makeOpAndRecordId(makeOpWithWallClockTime(3, 2 * kOneDayInMillis));
    _remoteOplog->setOperations({commonPoint});
    ASSERT_OK(_insertOplogEntry(commonPoint.first));
    ASSERT_OK(_insertOplogEntry(topOfOplog.first));

    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    // Run rollback.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
}

TEST_F(RollbackImplTest, RollbackKillsNecessaryOperations) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    transport::TransportLayerMock transportLayer;

    auto writeSession = transportLayer.createSession();
    auto writeClient =
        getGlobalServiceContext()->getService()->makeClient("writeClient", writeSession);
    auto writeOpCtx = writeClient->makeOperationContext();
    boost::optional<Lock::GlobalLock> globalWrite;
    globalWrite.emplace(writeOpCtx.get(), MODE_IX);
    ASSERT(globalWrite->isLocked());

    auto readSession = transportLayer.createSession();
    auto readClient =
        getGlobalServiceContext()->getService()->makeClient("readClient", readSession);
    auto readOpCtx = readClient->makeOperationContext();
    boost::optional<Lock::GlobalLock> globalRead;
    globalRead.emplace(readOpCtx.get(), MODE_IS);
    ASSERT(globalRead->isLocked());

    // Run rollback in a separate thread so the locking threads can check for interrupt.
    Status status(ErrorCodes::InternalError, "Not set");
    stdx::thread rollbackThread([&] {
        ThreadClient tc(getGlobalServiceContext()->getService());
        auto opCtx = tc.get()->makeOperationContext();
        status = _rollback->runRollback(opCtx.get());
    });

    while (!(writeOpCtx->isKillPending() && readOpCtx->isKillPending())) {
        // Do nothing.
        sleepmillis(10);
    }

    // We assume that an interrupted opCtx would release its locks.
    LOGV2(21649, "Both opCtx's marked for kill");
    ASSERT_EQ(ErrorCodes::InterruptedDueToReplStateChange, writeOpCtx->checkForInterruptNoAssert());
    globalWrite = boost::none;
    ASSERT_EQ(ErrorCodes::InterruptedDueToReplStateChange, readOpCtx->checkForInterruptNoAssert());
    globalRead = boost::none;
    LOGV2(21650, "Both opCtx's were interrupted");

    rollbackThread.join();
    ASSERT_OK(status);
}

TEST_F(RollbackImplTest, RollbackFailsIfRollbackPeriodIsTooLong) {

    auto commonPoint = makeOpAndRecordId(makeOpWithWallClockTime(1, 5 * 1000));
    auto opAfterCommonPoint = makeOpAndRecordId(makeOpWithWallClockTime(2, 5 * 1000));
    // We calculate the roll back time limit by comparing the difference between the top of the
    // oplog and the first oplog entry after the commit point. The default limit is 1 day, so we
    // make the difference be 2 days.
    auto topOfOplog = makeOpAndRecordId(makeOpWithWallClockTime(3, 2 * 60 * 60 * 24 * 1000));

    _remoteOplog->setOperations({commonPoint});
    ASSERT_OK(_insertOplogEntry(commonPoint.first));
    ASSERT_OK(_insertOplogEntry(opAfterCommonPoint.first));
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
    ASSERT_EQUALS(_truncatePoint, Timestamp(2, 2));
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

class RollbackTestingDurableHistoryPin : public DurableHistoryPin {
public:
    bool reconciled = false;

    RollbackTestingDurableHistoryPin(StorageInterface* storageInterface)
        : _storageInterface(storageInterface) {}

    void expectDocument(BSONObj&& obj) {
        _expectedDoc = std::move(obj);
    }

    std::string getName() override {
        return "testing";
    }

    // Test that documents inserted before rollback are still visible during execution of
    // DurableHistoryRegistry::reconcilePins()
    boost::optional<Timestamp> calculatePin(OperationContext* opCtx) override {
        auto document = _storageInterface->findById(opCtx, nss, _expectedDoc["_id"]);
        ASSERT_OK(document);
        ASSERT_BSONOBJ_EQ(_expectedDoc, document.getValue());
        reconciled = true;
        return _oldestTimestamp;
    }

private:
    StorageInterface* _storageInterface;
    BSONObj _expectedDoc;
    Timestamp _oldestTimestamp = Timestamp(0, 2);
};

TEST_F(RollbackImplTest, RollbackReconcilesHistoryPins) {
    // Test StorageInterfaceImpl::recoverToStableTimestamp instead of the mocked version
    _storageInterface->unmockStableTimestamp();
    auto uuid = kGenericUUID;
    auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    // Register the testing pin
    auto service = getServiceContext();
    DurableHistoryRegistry::set(service, std::make_unique<DurableHistoryRegistry>());
    auto uniquePinPointer = std::make_unique<RollbackTestingDurableHistoryPin>(_storageInterface);
    auto pin = uniquePinPointer.get();
    DurableHistoryRegistry::get(service)->registerPin(std::move(uniquePinPointer));

    // Insert a document at Timestamp(1, 1)
    auto obj = BSON("_id" << 0 << "name" << "kyle");
    _insertDocAndGenerateOplogEntry(obj, uuid, nss, 1);

    // Set stable timestamp at Timestamp(1, 1)
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    _storageInterface->setStableTimestamp(service, commonOp.first["ts"].timestamp());

    // Make sure rollback opened the catalog and reconciled the pins in the right order
    pin->expectDocument(std::move(obj));
    ASSERT_FALSE(pin->reconciled);
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    // Make sure calculatePin() was called
    ASSERT(pin->reconciled);
}

DEATH_TEST_REGEX_F(RollbackImplTest,
                   RollbackFassertsIfRecoverToStableTimestampFails,
                   "Fatal assertion.*4584700") {
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

    // Run rollback. It should fassert.
    _rollback->runRollback(_opCtx.get()).ignore();
}

TEST_F(RollbackImplTest, RollbackReturnsBadStatusIfIncrementRollbackIDFails) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    // Delete the rollback id collection.
    auto rollbackIdNss = NamespaceString::kDefaultRollbackIdNamespace;
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

TEST_F(RollbackImplTest,
       RollbackCannotBeShutDownBetweenAbortingAndReconstructingPreparedTransactions) {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    _assertDocsInOplog(_opCtx.get(), {1, 2});

    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    // Called before aborting prepared transactions. We request the shutdown here.
    _onRollbackIDIncrementedFn = [this]() {
        _incrementedRollbackID = true;
        _rollback->shutdown();
    };

    // Called after reconstructing prepared transactions.
    _onPreparedTransactionsReconstructedFn = [this]() {
        ASSERT(_incrementedRollbackID);
        _reconstructedPreparedTransactions = true;
    };

    // Shutting down is still allowed but it must occur after that window.
    ASSERT_EQ(ErrorCodes::ShutdownInProgress, _rollback->runRollback(_opCtx.get()));
    ASSERT(_incrementedRollbackID);
    ASSERT(_reconstructedPreparedTransactions);
}

DEATH_TEST_F(RollbackImplTest,
             RollbackUassertsAreFatalBetweenAbortingAndReconstructingPreparedTransactions,
             "UnknownError: error for test") {
    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));

    _assertDocsInOplog(_opCtx.get(), {1, 2});

    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    // Called before aborting prepared transactions.
    _onRollbackIDIncrementedFn = [this]() {
        _incrementedRollbackID = true;
    };

    _onRecoverToStableTimestampFn = [this](Timestamp stableTimestamp) {
        _recoveredToStableTimestamp = true;
        uasserted(ErrorCodes::UnknownError, "error for test");
    };

    // Called after reconstructing prepared transactions. We should not be getting here.
    _onPreparedTransactionsReconstructedFn = [this]() {
        ASSERT(false);
    };

    // We expect to crash when we hit the exception.
    _rollback->runRollback(_opCtx.get()).ignore();
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
        _opCtx.get(), {nss.dbName(), uuid}, {BSON("_id" << 2), time}, time.asULL()));
    ASSERT_EQ(2ULL,
              unittest::assertGet(
                  _storageInterface->getCollectionCount(_opCtx.get(), {nss.dbName(), uuid})));

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

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status.code());
    ASSERT(_recoveredToStableTimestamp);
    ASSERT(_recoveredFromOplog);
    ASSERT_FALSE(_triggeredOpObserver);

    // Make sure we transitioned back to SECONDARY state.
    ASSERT_EQUALS(_coordinator->getMemberState(), MemberState::RS_SECONDARY);

    _assertDocsInOplog(_opCtx.get(), {1});
    truncateAfterPoint =
        _replicationProcess->getConsistencyMarkers()->getOplogTruncateAfterPoint(_opCtx.get());
    ASSERT_EQUALS(Timestamp(), truncateAfterPoint);
    ASSERT_EQUALS(_truncatePoint, Timestamp(1, 1));
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
    ASSERT_EQUALS(_truncatePoint, Timestamp(1, 1));
}

DEATH_TEST_REGEX_F(RollbackImplTest,
                   RollbackTriggersFatalAssertionOnFailingToTransitionFromRollbackToSecondary,
                   "Failed to perform replica set state transition") {
    _coordinator->failSettingFollowerMode(MemberState::RS_SECONDARY, ErrorCodes::IllegalOperation);

    auto op = makeOpAndRecordId(1);
    _remoteOplog->setOperations({op});
    ASSERT_OK(_insertOplogEntry(op.first));
    ASSERT_OK(_insertOplogEntry(makeOp(2)));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    auto status = _rollback->runRollback(_opCtx.get());
    LOGV2(21651, "Mongod did not crash. Status: {status}", "status"_attr = status);
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
    const auto nss = NamespaceString::createNamespaceString_forTest("db.coll");
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    const auto oplogEntry = BSON("ts" << Timestamp(3, 3) << "t" << 3LL << "op"
                                      << "c"
                                      << "wall" << Date_t() << "o" << BSON("create" << nss.coll())
                                      << "ns" << nss.ns_forTest() << "ui" << uuid);
    ASSERT_OK(_insertOplogEntry(oplogEntry));

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT(_rollback->docsDeletedForNamespace_forTest(uuid).empty());
}

TEST_F(RollbackImplTest, RollbackSavesInsertedDocumentToFile) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nss = NamespaceString::createNamespaceString_forTest("db.people");
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

    const auto nss = NamespaceString::createNamespaceString_forTest("db.people");
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

    const auto nss = NamespaceString::createNamespaceString_forTest("db.people");
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

    const auto nss = NamespaceString::createNamespaceString_forTest("db.people");
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

    const auto nss = NamespaceString::createNamespaceString_forTest("db.numbers");
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

    const auto nss = NamespaceString::createNamespaceString_forTest("db.numbers");
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

    const auto nssBeforeRename = NamespaceString::createNamespaceString_forTest("db.firstColl");
    const auto uuidBeforeRename = UUID::gen();
    const auto collBeforeRename =
        _initializeCollection(_opCtx.get(), uuidBeforeRename, nssBeforeRename);

    // Insert a document into the collection.
    const auto objInRenamedCollection = BSON("_id" << "kyle");
    _insertDocAndGenerateOplogEntry(objInRenamedCollection, uuidBeforeRename, nssBeforeRename, 2);

    // Rename the original collection.
    const auto nssAfterRename = NamespaceString::createNamespaceString_forTest("db.secondColl");
    auto renameCmdObj = BSON("renameCollection" << nssBeforeRename.ns_forTest() << "to"
                                                << nssAfterRename.ns_forTest());
    auto renameCmdOp =
        makeCommandOp(Timestamp(3, 3), uuidBeforeRename, nssBeforeRename, renameCmdObj, 3);
    ASSERT_OK(_insertOplogEntry(renameCmdOp.first));
    ASSERT_OK(
        _storageInterface->renameCollection(_opCtx.get(), nssBeforeRename, nssAfterRename, true));

    // Create a new collection with the old name.
    const auto uuidAfterRename = UUID::gen();
    const auto collAfterRename =
        _initializeCollection(_opCtx.get(), uuidAfterRename, nssBeforeRename);

    // Insert a different document into the new collection.
    const auto objInNewCollection = BSON("_id" << "jungsoo");
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

TEST_F(RollbackImplTest, RollbackProperlySavesFilesWhenCreateCollAndInsertsAreRolledBack) {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    // Create the collection and make an oplog entry for the creation event.
    const auto nss = NamespaceString::createNamespaceString_forTest("db.people");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    const auto oplogEntry = BSON("ts" << Timestamp(3, 3) << "t" << 3LL << "op"
                                      << "c"
                                      << "wall" << Date_t() << "o" << BSON("create" << nss.coll())
                                      << "ns" << nss.ns_forTest() << "ui" << uuid);
    ASSERT_OK(_insertOplogEntry(oplogEntry));

    // Insert documents into the collection.
    const std::vector<BSONObj> objs(
        {BSON("_id" << "kyle"), BSON("_id" << "jungsoo"), BSON("_id" << "erjon")});
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

DEATH_TEST_F(RollbackImplTest,
             InvariantFailureIfNamespaceIsMissingWhenWritingRollbackFiles,
             "unexpectedly missing in the CollectionCatalog") {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto nss = NamespaceString::createNamespaceString_forTest("db.people");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto obj = BSON("_id" << 0 << "name"
                                << "kyle");
    _insertDocAndGenerateOplogEntry(obj, uuid, nss);

    // Drop the collection (immediately; not a two-phase drop), so that the namespace can no longer
    // be found. We enforce that the storage interface drops the collection immediately with an
    // unreplicated writes block, since unreplicated collection drops are not two-phase.
    {
        repl::UnreplicatedWritesBlock uwb(_opCtx.get());
        ASSERT_OK(_storageInterface->dropCollection(_opCtx.get(), nss));
    }

    auto status = _rollback->runRollback(_opCtx.get());
    LOGV2(21652, "mongod did not crash when expected; status: {status}", "status"_attr = status);
}

DEATH_TEST_F(RollbackImplTest,
             InvariantFailureIfNamespaceIsMissingWhenGettingCollectionSizes,
             "unexpectedly missing in the CollectionCatalog") {
    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    ASSERT_OK(_insertOplogEntry(
        makeDeleteOplogEntry(2, BSON("_id" << 1), nss.ns_forTest(), kGenericUUID)));

    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    auto status = _rollback->runRollback(_opCtx.get());
    LOGV2(21653, "mongod did not crash when expected; status: {status}", "status"_attr = status);
}

TEST_F(RollbackImplTest, RollbackSetsMultipleCollectionCounts) {
    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    const auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));

    auto uuid1 = UUID::gen();
    auto nss1 = NamespaceString::createNamespaceString_forTest("test.coll1");
    const auto obj1 = BSON("_id" << 1);
    const auto coll1 = _initializeCollection(_opCtx.get(), uuid1, nss1);
    _insertDocAndGenerateOplogEntry(obj1, uuid1, nss1, 2);

    const Timestamp time1 = Timestamp(2, 2);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss1.dbName(), uuid1}, {BSON("_id" << 2), time1}, time1.asULL()));
    ASSERT_EQ(2ULL,
              unittest::assertGet(
                  _storageInterface->getCollectionCount(_opCtx.get(), {nss1.dbName(), uuid1})));
    ASSERT_OK(_storageInterface->setCollectionCount(_opCtx.get(), {nss1.dbName(), uuid1}, 2));

    auto uuid2 = UUID::gen();
    auto nss2 = NamespaceString::createNamespaceString_forTest("test.coll2");
    const auto obj2 = BSON("_id" << 1);
    const auto coll2 = _initializeCollection(_opCtx.get(), uuid2, nss2);
    const Timestamp time2 = Timestamp(3, 3);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss2.dbName(), uuid2}, {obj2, time2}, time2.asULL()));
    _deleteDocAndGenerateOplogEntry(obj2["_id"], uuid2, nss2, 3);

    const Timestamp time3 = Timestamp(4, 4);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss2.dbName(), uuid2}, {BSON("_id" << 2), time3}, time3.asULL()));
    ASSERT_EQ(1ULL,
              unittest::assertGet(
                  _storageInterface->getCollectionCount(_opCtx.get(), {nss2.dbName(), uuid2})));
    ASSERT_OK(_storageInterface->setCollectionCount(_opCtx.get(), {nss2.dbName(), uuid2}, 1));

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
        _opCtx.get(), {nss.dbName(), uuid}, {obj, time}, time.asULL()));

    _insertDocAndGenerateOplogEntry(BSON("_id" << 1), uuid, nss, 2);
    _deleteDocAndGenerateOplogEntry(obj["_id"], uuid, nss, 3);
    _insertDocAndGenerateOplogEntry(BSON("_id" << 3), uuid, nss, 4);

    // Test that we read the collection count from drop entries.
    ASSERT_OK(_insertOplogEntry(makeCommandOp(Timestamp(5, 5),
                                              uuid,
                                              nss.getCommandNS(),
                                              BSON("drop" << nss.coll()),
                                              5,
                                              BSON("numRecords" << 1))
                                    .first));

    ASSERT_EQ(2ULL,
              unittest::assertGet(
                  _storageInterface->getCollectionCount(_opCtx.get(), {nss.dbName(), uuid})));
    ASSERT_OK(_storageInterface->setCollectionCount(nullptr, {DatabaseName(), uuid}, 2));

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
    auto nss1 = NamespaceString::createNamespaceString_forTest("test.coll1");
    const auto obj1 = BSON("_id" << 1);
    const auto coll1 = _initializeCollection(_opCtx.get(), uuid1, nss1);
    _insertDocAndGenerateOplogEntry(obj1, uuid1, nss1, 2);

    const Timestamp time1 = Timestamp(2, 2);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss1.dbName(), uuid1}, {BSON("_id" << 2), time1}, time1.asULL()));
    ASSERT_EQ(2ULL,
              unittest::assertGet(
                  _storageInterface->getCollectionCount(_opCtx.get(), {nss1.dbName(), uuid1})));
    ASSERT_OK(_storageInterface->setCollectionCount(nullptr, {DatabaseName(), uuid1}, 2));

    auto uuid2 = UUID::gen();
    auto nss2 = NamespaceString::createNamespaceString_forTest("test.coll2");
    const auto obj2 = BSON("_id" << 1);
    const auto coll2 = _initializeCollection(_opCtx.get(), uuid2, nss2);
    _insertDocAndGenerateOplogEntry(obj2, uuid2, nss2, 3);

    const Timestamp time2 = Timestamp(3, 3);
    ASSERT_OK(_storageInterface->insertDocument(
        _opCtx.get(), {nss2.dbName(), uuid2}, {BSON("_id" << 2), time2}, time2.asULL()));
    ASSERT_EQ(2ULL,
              unittest::assertGet(
                  _storageInterface->getCollectionCount(_opCtx.get(), {nss2.dbName(), uuid2})));
    ASSERT_OK(_storageInterface->setCollectionCount(nullptr, {DatabaseName(), uuid2}, 2));

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
    ASSERT_OK(_insertOplogEntry(
        makeInsertOplogEntry(2, BSON("_id" << 1), nss.ns_forTest(), kGenericUUID)));

    const auto coll = _initializeCollection(_opCtx.get(), kGenericUUID, nss);

    _storageInterface->setStableTimestamp(nullptr, Timestamp(1, 1));

    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    ASSERT_EQ(_storageInterface->getFinalCollectionCount(kGenericUUID), 0);
}

std::vector<OplogInterfaceMock::Operation>
RollbackImplTest::_setUpUnpreparedTransactionForCountTest(UUID collId) {
    std::vector<OplogInterfaceMock::Operation> ops;

    // Initialize the collection with one document inserted outside a transaction.
    // The final collection count after rolling back the transaction, which has one entry before the
    // stable timestamp, should be 1.
    auto nss = NamespaceString::createNamespaceString_forTest("test.coll1");
    _initializeCollection(_opCtx.get(), collId, nss);
    auto insertOp1 = _insertDocAndReturnOplogEntry(BSON("_id" << 1), collId, nss, 1);
    ops.push_back(insertOp1);
    ASSERT_OK(_insertOplogEntry(insertOp1.first));

    // Common field values for applyOps oplog entries.
    auto adminCmdNss =
        NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin).getCommandNS();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(makeLogicalSessionId(_opCtx.get()));
    sessionInfo.setTxnNumber(1);

    // Start an unprepared transaction with an applyOps oplog entry with the "partialTxn" field
    // set to true.
    auto insertOp2 = _insertDocAndReturnOplogEntry(BSON("_id" << 2), collId, nss, 2);
    auto partialApplyOpsOpTime = unittest::assertGet(OpTime::parseFromOplogEntry(insertOp2.first));

    // applyOps oplog entries cannot have an inner "ts", "t" or "wall" field.
    auto insertOp2Obj = insertOp2.first.removeField("ts");
    insertOp2Obj = insertOp2Obj.removeField("t");
    insertOp2Obj = insertOp2Obj.removeField("wall");

    auto partialApplyOpsObj = BSON("applyOps" << BSON_ARRAY(insertOp2Obj) << "partialTxn" << true);
    DurableOplogEntry partialApplyOpsOplogEntry(partialApplyOpsOpTime,  // opTime
                                                OpTypeEnum::kCommand,   // opType
                                                adminCmdNss,            // nss
                                                boost::none,            // uuid
                                                boost::none,            // fromMigrate
                                                boost::none,  // checkExistenceForDiffInsert
                                                boost::none,  // versionContext
                                                OplogEntry::kOplogVersion,  // version
                                                partialApplyOpsObj,         // oField
                                                boost::none,                // o2Field
                                                sessionInfo,                // sessionInfo
                                                boost::none,                // isUpsert
                                                Date_t(),                   // wallClockTime
                                                {},                         // statementIds
                                                OpTime(),      // prevWriteOpTimeInTransaction
                                                boost::none,   // preImageOpTime
                                                boost::none,   // postImageOpTime
                                                boost::none,   // ShardId of resharding recipient
                                                boost::none,   // _id
                                                boost::none);  // needsRetryImage
    ASSERT_OK(_insertOplogEntry(partialApplyOpsOplogEntry.toBSON()));
    ops.push_back(std::make_pair(partialApplyOpsOplogEntry.toBSON(), insertOp2.second));

    // Complete the unprepared transaction with an implicit commit oplog entry.
    // When rolling back the implicit commit operation, we should be traversing, in reverse, the
    // chain of applyOps oplog entries for this transaction.
    auto insertOp3 = _insertDocAndReturnOplogEntry(BSON("_id" << 3), collId, nss, 3);
    auto commitApplyOpsOpTime = unittest::assertGet(OpTime::parseFromOplogEntry(insertOp3.first));

    // applyOps oplog entries cannot have an inner "ts", "t" or "wall" field.
    auto insertOp3Obj = insertOp3.first.removeField("ts");
    insertOp3Obj = insertOp3Obj.removeField("t");
    insertOp3Obj = insertOp3Obj.removeField("wall");

    auto commitApplyOpsObj = BSON("applyOps" << BSON_ARRAY(insertOp3Obj) << "count" << 1);
    DurableOplogEntry commitApplyOpsOplogEntry(
        commitApplyOpsOpTime,       // opTime
        OpTypeEnum::kCommand,       // opType
        adminCmdNss,                // nss
        boost::none,                // uuid
        boost::none,                // fromMigrate
        boost::none,                // checkExistenceForDiffInsert
        boost::none,                // versionContext
        OplogEntry::kOplogVersion,  // version
        commitApplyOpsObj,          // oField
        boost::none,                // o2Field
        sessionInfo,                // sessionInfo
        boost::none,                // isUpsert
        Date_t(),                   // wallClockTime
        {},                         // statementIds
        partialApplyOpsOpTime,      // prevWriteOpTimeInTransaction
        boost::none,                // preImageOpTime
        boost::none,                // postImageOpTime
        boost::none,                // ShardId of resharding recipient
        boost::none,                // _id
        boost::none);               // needsRetryImage
    ASSERT_OK(_insertOplogEntry(commitApplyOpsOplogEntry.toBSON()));
    ops.push_back(std::make_pair(commitApplyOpsOplogEntry.toBSON(), insertOp3.second));

    ASSERT_OK(_storageInterface->setCollectionCount(nullptr, {nss.dbName(), collId}, 3));
    _assertDocsInOplog(_opCtx.get(), {1, 2, 3});

    return ops;
}

std::vector<OplogInterfaceMock::Operation>
RollbackImplTest::_setUpBatchedRetryableWriteForCountTest(UUID collId) {
    std::vector<OplogInterfaceMock::Operation> ops;

    // Initialize the collection with one document inserted outside a transaction.
    // The final collection count after partially rolling back the retryable write,
    // which has one entry before the stable timestamp, should be 2.
    auto nss = NamespaceString::createNamespaceString_forTest("test.coll1");
    _initializeCollection(_opCtx.get(), collId, nss);
    auto insertOp1 = _insertDocAndReturnOplogEntry(BSON("_id" << 1), collId, nss, 1);
    ops.push_back(insertOp1);
    ASSERT_OK(_insertOplogEntry(insertOp1.first));

    // Common field values for applyOps oplog entries.
    auto adminCmdNss =
        NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin).getCommandNS();
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(makeLogicalSessionId(_opCtx.get()));
    sessionInfo.setTxnNumber(1);

    // Start a retryable write with an applyOps oplog entry containing an insert op.
    auto insertOp2 = _insertDocAndReturnOplogEntry(BSON("_id" << 2), collId, nss, 2);
    auto applyOps0OpTime = unittest::assertGet(OpTime::parseFromOplogEntry(insertOp2.first));

    // applyOps oplog entries cannot have an inner "ts", "t" or "wall" field.
    auto insertOp2Obj = insertOp2.first.removeField("ts");
    insertOp2Obj = insertOp2Obj.removeField("t");
    insertOp2Obj = insertOp2Obj.removeField("wall");

    auto applyOps0Obj = BSON("applyOps" << BSON_ARRAY(insertOp2Obj));
    DurableOplogEntry applyOps0OplogEntry(applyOps0OpTime,            // opTime
                                          OpTypeEnum::kCommand,       // opType
                                          adminCmdNss,                // nss
                                          boost::none,                // uuid
                                          boost::none,                // fromMigrate
                                          boost::none,                // checkExistenceForDiffInsert
                                          boost::none,                // versionContext
                                          OplogEntry::kOplogVersion,  // version
                                          applyOps0Obj,               // oField
                                          boost::none,                // o2Field
                                          sessionInfo,                // sessionInfo
                                          boost::none,                // isUpsert
                                          Date_t(),                   // wallClockTime
                                          {},                         // statementIds
                                          OpTime(),      // prevWriteOpTimeInTransaction
                                          boost::none,   // preImageOpTime
                                          boost::none,   // postImageOpTime
                                          boost::none,   // ShardId of resharding recipient
                                          boost::none,   // _id
                                          boost::none);  // needsRetryImage
    auto applyOps0BSON = applyOps0OplogEntry.toBSON();
    applyOps0BSON = applyOps0BSON.addField(BSON("multiOpType" << 1).firstElement());
    ASSERT_OK(_insertOplogEntry(applyOps0BSON));
    ops.push_back(std::make_pair(applyOps0BSON, insertOp2.second));

    // Second entry in retryable write; this one should be rolled back.
    auto insertOp3 = _insertDocAndReturnOplogEntry(BSON("_id" << 3), collId, nss, 3);
    auto applyOps1OpTime = unittest::assertGet(OpTime::parseFromOplogEntry(insertOp3.first));

    // applyOps oplog entries cannot have an inner "ts", "t" or "wall" field.
    auto insertOp3Obj = insertOp3.first.removeField("ts");
    insertOp3Obj = insertOp3Obj.removeField("t");
    insertOp3Obj = insertOp3Obj.removeField("wall");

    auto applyOps1Obj = BSON("applyOps" << BSON_ARRAY(insertOp3Obj));
    DurableOplogEntry applyOps1OplogEntry(applyOps1OpTime,            // opTime
                                          OpTypeEnum::kCommand,       // opType
                                          adminCmdNss,                // nss
                                          boost::none,                // uuid
                                          boost::none,                // fromMigrate
                                          boost::none,                // checkExistenceForDiffInsert
                                          boost::none,                // versionContext
                                          OplogEntry::kOplogVersion,  // version
                                          applyOps1Obj,               // oField
                                          boost::none,                // o2Field
                                          sessionInfo,                // sessionInfo
                                          boost::none,                // isUpsert
                                          Date_t(),                   // wallClockTime
                                          {},                         // statementIds
                                          applyOps0OpTime,  // prevWriteOpTimeInTransaction
                                          boost::none,      // preImageOpTime
                                          boost::none,      // postImageOpTime
                                          boost::none,      // ShardId of resharding recipient
                                          boost::none,      // _id
                                          boost::none);     // needsRetryImage
    auto applyOps1BSON = applyOps1OplogEntry.toBSON();
    applyOps1BSON = applyOps1BSON.addField(BSON("multiOpType" << 1).firstElement());
    ASSERT_OK(_insertOplogEntry(applyOps1BSON));
    ops.push_back(std::make_pair(applyOps1BSON, insertOp3.second));

    ASSERT_OK(_storageInterface->setCollectionCount(nullptr, {nss.dbName(), collId}, 3));
    _assertDocsInOplog(_opCtx.get(), {1, 2, 3});

    return ops;
}

TEST_F(RollbackImplTest, RollbackFixesCountForUnpreparedTransactionApplyOpsChain1) {
    auto collId = UUID::gen();
    auto ops = _setUpUnpreparedTransactionForCountTest(collId);

    // Make the non-transaction CRUD oplog entry the common point and use its timestamp for the
    // stable timestamp.
    const auto& commonOp = ops[0];
    _storageInterface->setStableTimestamp(nullptr, commonOp.first["ts"].timestamp());
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // The entire applyOps chain occurs after the common point.
    ASSERT_EQ(_storageInterface->getFinalCollectionCount(collId), 1);
}

TEST_F(RollbackImplTest, RollbackFixesCountForUnpreparedTransactionApplyOpsChain2) {
    auto collId = UUID::gen();
    auto ops = _setUpUnpreparedTransactionForCountTest(collId);

    // Make the starting applyOps oplog entry the common point and use its timestamp for the stable
    // timestamp.
    const auto& commonOp = ops[1];
    _storageInterface->setStableTimestamp(nullptr, commonOp.first["ts"].timestamp());
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    ASSERT_EQ(_storageInterface->getFinalCollectionCount(collId), 1);
}

TEST_F(RollbackImplTest, RollbackFixesCountForBatchedRetryableWriteApplyOpsChain1) {
    auto collId = UUID::gen();
    auto ops = _setUpBatchedRetryableWriteForCountTest(collId);

    // Make the non-transaction CRUD oplog entry the common point and use its timestamp for the
    // stable timestamp.
    const auto& commonOp = ops[0];
    _storageInterface->setStableTimestamp(nullptr, commonOp.first["ts"].timestamp());
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    // The entire applyOps chain occurs after the common point.
    ASSERT_EQ(_storageInterface->getFinalCollectionCount(collId), 1);
}

TEST_F(RollbackImplTest, RollbackFixesCountForBatchedRetryableWriteApplyOpsChain2) {
    const auto nss = NamespaceString::kSessionTransactionsTableNamespace;
    _initializeCollection(_opCtx.get(), UUID::gen(), nss);

    auto collId = UUID::gen();
    auto ops = _setUpBatchedRetryableWriteForCountTest(collId);

    // Make the starting applyOps oplog entry the common point and use its timestamp for the stable
    // timestamp.
    const auto& commonOp = ops[1];
    _storageInterface->setStableTimestamp(nullptr, commonOp.first["ts"].timestamp());
    _remoteOplog->setOperations({commonOp});
    // The 'config.transactions' table is currently empty.
    ASSERT_NOT_OK(_storageInterface->findSingleton(_opCtx.get(), nss));
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));

    ASSERT_EQ(_storageInterface->getFinalCollectionCount(collId), 2);

    auto swDoc = _storageInterface->findById(_opCtx.get(), nss, commonOp.first.getField("lsid"));
    ASSERT_OK(swDoc);
    auto sessionsEntryBson = swDoc.getValue();
    // New sessions entry should match the session information retrieved from the retryable writes
    // oplog entry from the 'stableTimestamp'.
    ASSERT_EQUALS(sessionsEntryBson["txnNum"].numberInt(), commonOp.first["txnNumber"].numberInt());
    ASSERT_EQ(sessionsEntryBson["lastWriteOpTime"]["ts"].timestamp(),
              commonOp.first["ts"].timestamp());
    ASSERT_EQ(sessionsEntryBson["lastWriteOpTime"]["t"].numberInt(),
              commonOp.first["t"].numberInt());
}

TEST_F(RollbackImplTest, RollbackRestoresTxnTableEntryToBeConsistentWithStableTimestamp) {
    const auto collUuid = UUID::gen();
    const auto nss = NamespaceString::kSessionTransactionsTableNamespace;
    _initializeCollection(_opCtx.get(), collUuid, nss);
    LogicalSessionFromClient fromClient{};
    fromClient.setId(UUID::gen());
    LogicalSessionId lsid = makeLogicalSessionId(fromClient, _opCtx.get());
    LogicalSessionFromClient fromClient2{};
    fromClient2.setId(UUID::gen());
    LogicalSessionId lsid2 = makeLogicalSessionId(fromClient2, _opCtx.get());
    LogicalSessionFromClient fromClient3{};
    fromClient3.setId(UUID::gen());
    LogicalSessionId lsid3 = makeLogicalSessionId(fromClient3, _opCtx.get());

    auto commonOpTime = OpTime(Timestamp(4, 4), 4);
    auto commonPoint = makeOpAndRecordId(commonOpTime);
    _remoteOplog->setOperations({commonPoint});
    _storageInterface->setStableTimestamp(nullptr, commonOpTime.getTimestamp());

    auto insertObj1 = BSON("_id" << 1);
    auto insertObj2 = BSON("_id" << 2);
    const auto txnNumOne = 1LL;
    // Create retryable write oplog entry before 'stableTimestamp'.
    auto opBeforeStableTs = makeInsertOplogEntry(1, insertObj1, redactTenant(nss), collUuid);
    const auto prevOpTime = OpTime(opBeforeStableTs["ts"].timestamp(), 1);
    BSONObjBuilder opBeforeStableTsBuilder(opBeforeStableTs);
    opBeforeStableTsBuilder.append("lsid", lsid.toBSON());
    opBeforeStableTsBuilder.append("txnNumber", txnNumOne);
    opBeforeStableTsBuilder.append("prevOpTime", OpTime().toBSON());
    opBeforeStableTsBuilder.append("stmtId", 1);
    BSONObj oplogEntryBeforeStableTs = opBeforeStableTsBuilder.done();

    const auto txnNumTwo = 2LL;
    // Create no-op retryable write entry before 'stableTimestamp'.
    auto noopPrevOpTime = OpTime(Timestamp(2, 2), 2);
    auto noopEntryBeforeStableTs = makeMigratedNoop(noopPrevOpTime,
                                                    oplogEntryBeforeStableTs,
                                                    lsid2,
                                                    txnNumTwo,
                                                    OpTime(),
                                                    1 /* stmtId */,
                                                    2 /* wallClockMillis */,
                                                    true /* isRetryableWrite */);

    // Create transactions entry after 'stableTimestamp'. Transactions entries are of 'command' op
    // type.
    auto txnOpTime = OpTime(Timestamp(3, 3), 3);
    auto txnEntryBeforeStableTs = makeTransactionOplogEntry(txnOpTime, lsid3, 3, OpTime());

    // Create retryable write oplog entry after 'stableTimestamp'.
    auto firstOpAfterStableTs = makeInsertOplogEntry(5, insertObj2, redactTenant(nss), collUuid);
    BSONObjBuilder opAfterStableTsBuilder(firstOpAfterStableTs);
    opAfterStableTsBuilder.append("lsid", lsid.toBSON());
    opAfterStableTsBuilder.append("txnNumber", txnNumOne);
    // 'prevOpTime' points to 'oplogEntryBeforeStableTs'.
    opAfterStableTsBuilder.append("prevOpTime", prevOpTime.toBSON());
    opAfterStableTsBuilder.append("stmtId", 2);
    BSONObj firstOplogEntryAfterStableTs = opAfterStableTsBuilder.done();

    // Create no-op retryable write entry after 'stableTimestamp'.
    auto noopEntryAfterStableTs = makeMigratedNoop(OpTime(Timestamp(6, 6), 6),
                                                   firstOplogEntryAfterStableTs,
                                                   lsid2,
                                                   txnNumTwo,
                                                   noopPrevOpTime,
                                                   2 /* stmtId */,
                                                   5 /* wallClockMillis */,
                                                   true /* isRetryableWrite */);

    // Create transactions entry after 'stableTimestamp'. Transactions entries are of 'command' op
    // type.
    auto txnEntryAfterStableTs =
        makeTransactionOplogEntry(OpTime(Timestamp(7, 7), 7), lsid3, 3, txnOpTime);

    ASSERT_OK(_insertOplogEntry(oplogEntryBeforeStableTs));
    ASSERT_OK(_insertOplogEntry(noopEntryBeforeStableTs));
    ASSERT_OK(_insertOplogEntry(txnEntryBeforeStableTs));
    ASSERT_OK(_insertOplogEntry(commonPoint.first));
    ASSERT_OK(_insertOplogEntry(firstOplogEntryAfterStableTs));
    ASSERT_OK(_insertOplogEntry(noopEntryAfterStableTs));
    ASSERT_OK(_insertOplogEntry(txnEntryAfterStableTs));

    auto status = _storageInterface->findSingleton(_opCtx.get(), nss);
    // The 'config.transactions' table is currently empty.
    ASSERT_NOT_OK(status);

    // Doing a rollback should upsert two entries into the 'config.transactions' table.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    auto swDoc = _storageInterface->findById(
        _opCtx.get(), nss, firstOplogEntryAfterStableTs.getField("lsid"));
    ASSERT_OK(swDoc);
    auto sessionsEntryBson = swDoc.getValue();
    // New sessions entry should match the session information retrieved from the retryable writes
    // oplog entry from before the 'stableTimestamp'.
    ASSERT_EQUALS(sessionsEntryBson["txnNum"].numberInt(),
                  oplogEntryBeforeStableTs["txnNumber"].numberInt());
    ASSERT_EQUALS(sessionsEntryBson["lastWriteOpTime"].timestamp(),
                  oplogEntryBeforeStableTs["prevOpTime"].timestamp());
    ASSERT_EQUALS(sessionsEntryBson["lastWriteDate"].date(),
                  oplogEntryBeforeStableTs["wall"].date());

    swDoc =
        _storageInterface->findById(_opCtx.get(), nss, noopEntryBeforeStableTs.getField("lsid"));
    ASSERT_OK(swDoc);
    sessionsEntryBson = swDoc.getValue();
    // New sessions entry should match the session information retrieved from the noop retryable
    // writes oplog entry from before the 'stableTimestamp'.
    ASSERT_EQUALS(sessionsEntryBson["txnNum"].numberInt(),
                  noopEntryBeforeStableTs["txnNumber"].numberInt());
    ASSERT_EQUALS(sessionsEntryBson["lastWriteOpTime"].timestamp(),
                  noopEntryBeforeStableTs["prevOpTime"].timestamp());
    ASSERT_EQUALS(sessionsEntryBson["lastWriteDate"].date(),
                  noopEntryBeforeStableTs["wall"].date());

    // 'lsid3' does not get restored because it is not a retryable writes entry.
    status = _storageInterface->findById(_opCtx.get(), nss, txnEntryAfterStableTs.getField("lsid"));
    ASSERT_NOT_OK(status);
}

TEST_F(
    RollbackImplTest,
    RollbackRestoresTxnTableEntryToBeConsistentWithStableTimestampWithMissingPrevWriteOplogEntry) {
    const auto collUuid = UUID::gen();
    const auto nss = NamespaceString::kSessionTransactionsTableNamespace;
    _initializeCollection(_opCtx.get(), collUuid, nss);
    LogicalSessionFromClient fromClient{};
    fromClient.setId(UUID::gen());
    LogicalSessionId lsid = makeLogicalSessionId(fromClient, _opCtx.get());
    LogicalSessionFromClient fromClient2{};
    fromClient2.setId(UUID::gen());
    LogicalSessionId lsid2 = makeLogicalSessionId(fromClient2, _opCtx.get());

    auto commonOpTime = OpTime(Timestamp(2, 2), 1);
    auto commonPoint = makeOpAndRecordId(OpTime(Timestamp(2, 2), 1));
    _remoteOplog->setOperations({commonPoint});
    _storageInterface->setStableTimestamp(nullptr, commonOpTime.getTimestamp());

    // Create oplog entry after 'stableTimestamp'.
    auto insertObj = BSON("_id" << 1);
    auto firstOpAfterStableTs = makeInsertOplogEntry(3, insertObj, redactTenant(nss), collUuid);
    BSONObjBuilder builder(firstOpAfterStableTs);
    builder.append("lsid", lsid.toBSON());
    builder.append("txnNumber", 2LL);
    // 'prevOpTime' points to an opTime not found in the oplog. This can happen in practice
    // due to the 'OplogCapMaintainerThread' truncating entries from before the 'stableTimestamp'.
    builder.append("prevOpTime", OpTime(Timestamp(1, 0), 1).toBSON());
    builder.append("stmtId", 2);
    BSONObj firstOplogEntryAfterStableTs = builder.done();

    // Create no-op entry after 'stableTimestamp'.
    // 'prevOpTime' point sto an opTime not found in the oplog.
    auto noopEntryAfterStableTs = makeMigratedNoop(OpTime(Timestamp(5, 5), 5),
                                                   firstOplogEntryAfterStableTs,
                                                   lsid2,
                                                   3 /* txnNum */,
                                                   OpTime(Timestamp(2, 1), 1),
                                                   2 /* stmtId */,
                                                   5 /* wallClockMillis */,
                                                   true /*isRetryableWrite */);

    ASSERT_OK(_insertOplogEntry(commonPoint.first));
    ASSERT_OK(_insertOplogEntry(firstOplogEntryAfterStableTs));
    ASSERT_OK(_insertOplogEntry(noopEntryAfterStableTs));

    auto status = _storageInterface->findSingleton(_opCtx.get(), nss);
    // The 'config.transactions' table is currently empty.
    ASSERT_NOT_OK(status);

    // Doing a rollback should upsert two entries into the 'config.transactions' table.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    auto swDoc = _storageInterface->findById(
        _opCtx.get(), nss, firstOplogEntryAfterStableTs.getField("lsid"));
    ASSERT_OK(swDoc);
    auto sessionsEntryBson = swDoc.getValue();
    // New sessions entry should match the session information retrieved from the retryable writes
    // oplog entry from after the 'stableTimestamp'.
    ASSERT_EQUALS(sessionsEntryBson["txnNum"].numberInt(),
                  firstOplogEntryAfterStableTs["txnNumber"].numberInt());
    ASSERT_EQUALS(sessionsEntryBson["lastWriteOpTime"].timestamp(),
                  firstOplogEntryAfterStableTs["prevOpTime"].timestamp());
    ASSERT_EQUALS(sessionsEntryBson["lastWriteDate"].date(),
                  firstOplogEntryAfterStableTs["wall"].date());

    swDoc = _storageInterface->findById(_opCtx.get(), nss, noopEntryAfterStableTs.getField("lsid"));
    ASSERT_OK(swDoc);
    sessionsEntryBson = swDoc.getValue();
    // New sessions entry should match the session information retrieved from the retryable writes
    // no-op oplog entry from after the 'stableTimestamp'.
    ASSERT_EQUALS(sessionsEntryBson["txnNum"].numberInt(),
                  noopEntryAfterStableTs["txnNumber"].numberInt());
    ASSERT_EQUALS(sessionsEntryBson["lastWriteOpTime"].timestamp(),
                  noopEntryAfterStableTs["prevOpTime"].timestamp());
    ASSERT_EQUALS(sessionsEntryBson["lastWriteDate"].date(), noopEntryAfterStableTs["wall"].date());
}

TEST_F(RollbackImplTest, RollbackDoesNotRestoreTxnsTableWhenNoRetryableWritesEntriesAfterStableTs) {
    const auto collUuid = UUID::gen();
    const auto nss = NamespaceString::kSessionTransactionsTableNamespace;
    _initializeCollection(_opCtx.get(), collUuid, nss);
    LogicalSessionFromClient fromClient{};
    fromClient.setId(UUID::gen());
    LogicalSessionId lsid = makeLogicalSessionId(fromClient, _opCtx.get());
    LogicalSessionFromClient fromClient2{};
    fromClient2.setId(UUID::gen());
    LogicalSessionId lsid2 = makeLogicalSessionId(fromClient2, _opCtx.get());

    auto commonOpTime = OpTime(Timestamp(4, 4), 4);
    auto commonPoint = makeOpAndRecordId(commonOpTime);
    _remoteOplog->setOperations({commonPoint});
    _storageInterface->setStableTimestamp(nullptr, commonOpTime.getTimestamp());

    auto insertObj1 = BSON("_id" << 1);
    auto insertObj2 = BSON("_id" << 2);
    const auto txnNumOne = 1LL;
    // Create oplog entry before 'stableTimestamp'.
    auto opBeforeStableTs = makeInsertOplogEntry(1, insertObj1, redactTenant(nss), collUuid);
    BSONObjBuilder opBeforeStableTsBuilder(opBeforeStableTs);
    opBeforeStableTsBuilder.append("lsid", lsid.toBSON());
    opBeforeStableTsBuilder.append("txnNumber", txnNumOne);
    opBeforeStableTsBuilder.append("prevOpTime", OpTime().toBSON());
    opBeforeStableTsBuilder.append("stmtId", 1);
    BSONObj oplogEntryBeforeStableTs = opBeforeStableTsBuilder.done();

    const auto txnNumTwo = 2LL;
    // Create no-op entry before 'stableTimestamp'.
    auto noopEntryBeforeStableTs = makeMigratedNoop(OpTime(Timestamp(2, 2), 2),
                                                    oplogEntryBeforeStableTs,
                                                    lsid2,
                                                    txnNumTwo,
                                                    OpTime(),
                                                    1 /* stmtId*/,
                                                    2 /* wallClockMillis */,
                                                    true /* isRetryableWrite */);

    // Create migrated no-op transactions entry before 'stableTimestamp'.
    auto txnOpTime = OpTime(Timestamp(3, 3), 3);
    auto txnEntryBeforeStableTs = makeMigratedNoop(txnOpTime,
                                                   BSONObj(),
                                                   lsid,
                                                   txnNumTwo,
                                                   OpTime(),
                                                   boost::none /* stmtId */,
                                                   4 /* wallClockMillis */,
                                                   false /* isRetryableWrite */);

    // Create regular non-retryable write oplog entry after 'stableTimestamp'.
    auto insertEntry = makeInsertOplogEntry(7, BSON("_id" << 3), redactTenant(nss), collUuid);

    // Create migrated no-op transactions entry after 'stableTimestamp'.
    auto txnEntryAfterStableTs = makeMigratedNoop({{8, 8}, 8},
                                                  BSONObj(),
                                                  lsid,
                                                  txnNumTwo,
                                                  txnOpTime,
                                                  boost::none /* stmtId */,
                                                  10 /* wallClockMillis */,
                                                  false /* isRetryableWrite */);

    ASSERT_OK(_insertOplogEntry(oplogEntryBeforeStableTs));
    ASSERT_OK(_insertOplogEntry(noopEntryBeforeStableTs));
    ASSERT_OK(_insertOplogEntry(txnEntryBeforeStableTs));
    ASSERT_OK(_insertOplogEntry(commonPoint.first));
    ASSERT_OK(_insertOplogEntry(insertEntry));
    ASSERT_OK(_insertOplogEntry(txnEntryAfterStableTs));

    auto status = _storageInterface->findSingleton(_opCtx.get(), nss);
    // The 'config.transactions' table is currently empty.
    ASSERT_NOT_OK(status);

    // Doing a rollback should not restore any entries into the 'config.transactions' table.
    ASSERT_OK(_rollback->runRollback(_opCtx.get()));
    status = _storageInterface->findSingleton(_opCtx.get(), nss);
    // No upserts were made to the 'config.transactions' table.
    ASSERT_NOT_OK(status);
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
        _storageInterface->oplogDiskLocRegister(
            _opCtx.get(), ops.front().first["ts"].timestamp(), true);
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
            _opCtx.get(), {nss.dbName(), collId}, {doc, time}, time.asULL()));

        BSONObjBuilder bob;
        bob.append("ts", time);
        bob.append("op", "i");
        collId.appendToBuilder(&bob, "ui");
        bob.append("ns", nss.ns_forTest());
        bob.append("o", doc);
        bob.append("wall", Date_t());
        bob.append("lsid",
                   BSON("id" << sessionId << "uid"
                             << BSONBinData(std::string(32, 'x').data(), 32, BinDataGeneral)));
        bob.append("txnNumber", txnNum);
        return bob.obj();
    }

    void assertRollbackInfoContainsObjectForUUID(UUID uuid, BSONObj bson) {
        const auto& uuidToIdMap = _rbInfo.rollbackDeletedIdsMap;
        auto search = uuidToIdMap.find(uuid);
        ASSERT(search != uuidToIdMap.end())
            << "map is unexpectedly missing an entry for uuid " << uuid.toString()
            << " containing object " << bson.jsonString(JsonStringFormat::LegacyStrict);
        const auto& idObjSet = search->second;
        const auto iter = idObjSet.find(bson);
        ASSERT(iter != idObjSet.end()) << "_id object set is unexpectedly missing object "
                                       << bson.jsonString(JsonStringFormat::LegacyStrict)
                                       << " in namespace with uuid " << uuid.toString();
    }

protected:
    OpObserver::RollbackObserverInfo _rbInfo;
};

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDsForOpsExtractsNamespaceAndUUIDOfInsertOplogEntry) {
    auto insertNss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto uuid = UUID::gen();
    auto ts = Timestamp(2, 2);
    auto insertOp = makeCRUDOp(
        OpTypeEnum::kInsert, ts, uuid, insertNss.ns_forTest(), BSON("_id" << 1), boost::none, 2);

    std::set<NamespaceString> expectedNamespaces = {insertNss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] = unittest::assertGet(
        _rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(insertOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDsForOpsExtractsNamespaceAndUUIDOfUpdateOplogEntry) {
    auto updateNss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto uuid = UUID::gen();
    auto ts = Timestamp(2, 2);
    auto o = BSON("$set" << BSON("x" << 2));
    auto updateOp =
        makeCRUDOp(OpTypeEnum::kUpdate, ts, uuid, updateNss.ns_forTest(), o, BSON("_id" << 1), 2);

    std::set<NamespaceString> expectedNamespaces = {updateNss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] = unittest::assertGet(
        _rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(updateOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDsForOpsExtractsNamespaceAndUUIDOfDeleteOplogEntry) {
    auto deleteNss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto uuid = UUID::gen();
    auto ts = Timestamp(2, 2);
    auto deleteOp = makeCRUDOp(
        OpTypeEnum::kDelete, ts, uuid, deleteNss.ns_forTest(), BSON("_id" << 1), boost::none, 2);

    std::set<NamespaceString> expectedNamespaces = {deleteNss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] = unittest::assertGet(
        _rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(deleteOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDsForOpsIgnoresNamespaceAndUUIDOfNoopOplogEntry) {
    auto noopNss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto ts = Timestamp(2, 2);
    auto noop = makeCRUDOp(
        OpTypeEnum::kNoop, ts, UUID::gen(), noopNss.ns_forTest(), BSONObj(), boost::none, 2);

    std::set<NamespaceString> expectedNamespaces = {};
    std::set<UUID> expectedUUIDs = {};
    auto [namespaces, uuids] =
        unittest::assertGet(_rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(noop.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDsForOpsExtractsNamespaceAndUUIDOfCreateCollectionOplogEntry) {
    auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto uuid = UUID::gen();
    auto cmdOp =
        makeCommandOp(Timestamp(2, 2), uuid, nss.getCommandNS(), BSON("create" << nss.coll()), 2);
    std::set<NamespaceString> expectedNamespaces = {nss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] =
        unittest::assertGet(_rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDsForOpsExtractsNamespaceAndUUIDOfDropCollectionOplogEntry) {
    auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto uuid = UUID::gen();
    auto cmdOp =
        makeCommandOp(Timestamp(2, 2), uuid, nss.getCommandNS(), BSON("drop" << nss.coll()), 2);

    std::set<NamespaceString> expectedNamespaces = {nss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] =
        unittest::assertGet(_rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDsForOpsExtractsNamespaceAndUUIDOfCreateIndexOplogEntry) {
    auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto uuid = UUID::gen();

    BSONObj indexObj = BSON("createIndexes" << nss.coll() << "v" << 2 << "key"
                                            << "x"
                                            << "name"
                                            << "x_1");
    auto cmdOp = makeCommandOp(Timestamp(2, 2), uuid, nss.getCommandNS(), indexObj, 2);

    std::set<NamespaceString> expectedNamespaces = {nss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] =
        unittest::assertGet(_rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDsForOpsExtractsNamespaceAndUUIDOfDropIndexOplogEntry) {
    auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto uuid = UUID::gen();
    auto cmdOp = makeCommandOp(Timestamp(2, 2),
                               uuid,
                               nss.getCommandNS(),
                               BSON("dropIndexes" << nss.coll() << "index"
                                                  << "x_1"),
                               2);
    std::set<NamespaceString> expectedNamespaces = {nss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] =
        unittest::assertGet(_rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDsForOpsExtractsNamespacesAndUUIDOfRenameCollectionOplogEntry) {
    auto fromNss = NamespaceString::createNamespaceString_forTest("test", "source");
    auto toNss = NamespaceString::createNamespaceString_forTest("test", "dest");
    auto uuid = UUID::gen();

    auto cmdObj = BSON("renameCollection" << fromNss.ns_forTest() << "to" << toNss.ns_forTest());
    auto cmdOp = makeCommandOp(Timestamp(2, 2), uuid, fromNss.getCommandNS(), cmdObj, 2);

    std::set<NamespaceString> expectedNamespaces = {fromNss, toNss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] =
        unittest::assertGet(_rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(
    RollbackImplObserverInfoTest,
    NamespacesAndUUIDsForOpsExtractsNamespacesAndUUIDOfRenameCollectionOplogEntryWithMultitenancy) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);

    boost::optional<TenantId> tid(OID::gen());
    auto fromNss = NamespaceString::createNamespaceString_forTest(tid, "test", "source");
    auto toNss = NamespaceString::createNamespaceString_forTest(tid, "test", "dest");
    auto uuid = UUID::gen();

    auto cmdObj = BSON("renameCollection" << NamespaceStringUtil::serialize(
                                                 fromNss, SerializationContext::stateDefault())
                                          << "to"
                                          << NamespaceStringUtil::serialize(
                                                 toNss, SerializationContext::stateDefault()));
    auto cmdOp = makeCommandOp(Timestamp(2, 2), uuid, fromNss, cmdObj, 2);


    std::set<NamespaceString> expectedNamespaces = {fromNss, toNss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] =
        unittest::assertGet(_rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(
    RollbackImplObserverInfoTest,
    NamespacesAndUUIDsForOpsExtractsNamespacesAndUUIDOfRenameCollectionOplogEntryRequiresTenantId) {
    RAIIServerParameterControllerForTest multitenancyController("multitenancySupport", true);
    RAIIServerParameterControllerForTest featureFlagController("featureFlagRequireTenantID", true);

    boost::optional<TenantId> tid(OID::gen());
    auto fromNss = NamespaceString::createNamespaceString_forTest(tid, "test", "source");
    auto toNss = NamespaceString::createNamespaceString_forTest(tid, "test", "dest");
    auto uuid = UUID::gen();

    auto cmdObj = BSON("renameCollection" << NamespaceStringUtil::serialize(
                                                 fromNss, SerializationContext::stateDefault())
                                          << "to"
                                          << NamespaceStringUtil::serialize(
                                                 toNss, SerializationContext::stateDefault()));
    auto cmdOp = makeCommandOp(Timestamp(2, 2), uuid, fromNss, cmdObj, 2, boost::none, tid);

    std::set<NamespaceString> expectedNamespaces = {fromNss, toNss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] =
        unittest::assertGet(_rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDForOpsIgnoresNamespaceAndUUIDOfDropDatabaseOplogEntry) {
    auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto cmdObj = BSON("dropDatabase" << 1);
    auto cmdOp = makeCommandOp(Timestamp(2, 2), boost::none, nss.getCommandNS(), cmdObj, 2);

    std::set<NamespaceString> expectedNamespaces = {};
    std::set<UUID> expectedUUIDs = {};
    auto [namespaces, uuids] =
        unittest::assertGet(_rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

TEST_F(RollbackImplObserverInfoTest,
       NamespacesAndUUIDsForOpsExtractsNamespacesAndUUIDOfCollModOplogEntry) {
    auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    auto uuid = UUID::gen();
    auto cmdObj = BSON("collMod" << nss.coll() << "validationLevel"
                                 << "off");
    auto cmdOp = makeCommandOp(Timestamp(2, 2), uuid, nss.getCommandNS(), cmdObj, 2);

    std::set<NamespaceString> expectedNamespaces = {nss};
    std::set<UUID> expectedUUIDs = {uuid};
    auto [namespaces, uuids] =
        unittest::assertGet(_rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(cmdOp.first)));
    ASSERT(expectedNamespaces == namespaces);
    ASSERT(expectedUUIDs == uuids);
}

DEATH_TEST_F(RollbackImplObserverInfoTest,
             NamespacesForOpsInvariantsOnApplyOpsOplogEntry,
             "_namespacesAndUUIDsForOp does not handle 'applyOps' oplog entries.") {
    // Add one sub-op.
    auto createNss = NamespaceString::createNamespaceString_forTest("test", "createColl");
    auto createOp = makeCommandOp(Timestamp(2, 2),
                                  UUID::gen(),
                                  createNss.getCommandNS(),
                                  BSON("create" << createNss.coll()),
                                  2);

    // Create the applyOps command object.
    BSONArrayBuilder subops;
    subops.append(createOp.first);
    auto applyOpsCmdOp =
        makeCommandOp(Timestamp(2, 2),
                      boost::none,
                      NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin, "$cmd"),
                      BSON("applyOps" << subops.arr()),
                      2);

    auto status = _rollback->_namespacesAndUUIDsForOp_forTest(OplogEntry(applyOpsCmdOp.first));
    LOGV2(21654,
          "Mongod did not crash. Status: {status_getStatus}",
          "status_getStatus"_attr = status.getStatus());
    MONGO_UNREACHABLE;
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsNamespacesAndUUIDsOfApplyOpsOplogEntry) {

    // Add a few different sub-ops from different namespaces to make sure they all get recorded.
    auto createNss = NamespaceString::createNamespaceString_forTest("test", "createColl");
    auto createUUID = UUID::gen();
    auto createOp = makeCommandOpForApplyOps(createUUID,
                                             createNss.getCommandNS().toString_forTest(),
                                             BSON("create" << createNss.coll()),
                                             2);

    auto dropNss = NamespaceString::createNamespaceString_forTest("test", "dropColl");
    auto dropUUID = UUID::gen();
    auto dropOp = makeCommandOpForApplyOps(
        dropUUID, dropNss.getCommandNS().toString_forTest(), BSON("drop" << dropNss.coll()), 2);
    _initializeCollection(_opCtx.get(), dropUUID, dropNss);

    auto collModNss = NamespaceString::createNamespaceString_forTest("test", "collModColl");
    auto collModUUID = UUID::gen();
    auto collModOp =
        makeCommandOpForApplyOps(collModUUID,
                                 collModNss.getCommandNS().ns_forTest(),
                                 BSON("collMod" << collModNss.coll() << "validationLevel"
                                                << "off"),
                                 2);

    auto renameFromNss = NamespaceString::createNamespaceString_forTest("test", "fromColl");
    auto renameToNss = NamespaceString::createNamespaceString_forTest("test", "toColl");
    auto renameFromUUID = UUID::gen();
    auto dropTargetUUID = UUID::gen();
    const auto collBeforeRename =
        _initializeCollection(_opCtx.get(), renameFromUUID, renameFromNss);
    const auto collToDrop = _initializeCollection(_opCtx.get(), dropTargetUUID, renameToNss);
    auto renameOp = makeCommandOpForApplyOps(
        renameFromUUID,
        renameFromNss.getCommandNS().ns_forTest(),
        BSON("renameCollection" << renameFromNss.ns_forTest() << "to" << renameToNss.ns_forTest()
                                << "dropTarget" << dropTargetUUID << "validationLevel"
                                << "off"),
        2);

    // Create the applyOps command object.
    BSONArrayBuilder subops;
    subops.append(createOp.first);
    subops.append(dropOp.first);
    subops.append(collModOp.first);
    subops.append(renameOp.first);
    auto applyOpsCmdOp =
        makeCommandOp(Timestamp(2, 2),
                      boost::none,
                      NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin, "$cmd"),
                      BSON("applyOps" << subops.arr()),
                      2);

    ASSERT_OK(rollbackOps({applyOpsCmdOp}));
    std::set<NamespaceString> expectedNamespaces = {
        createNss, dropNss, collModNss, renameFromNss, renameToNss};
    std::set<UUID> expectedUUIDs = {
        createUUID, dropUUID, collModUUID, renameFromUUID, dropTargetUUID};
    invariant(expectedNamespaces == _rbInfo.rollbackNamespaces);
    invariant(expectedUUIDs == _rbInfo.rollbackUUIDs);
}

TEST_F(RollbackImplObserverInfoTest, RollbackFailsOnMalformedApplyOpsOplogEntry) {
    // Make the argument to the 'applyOps' command an object instead of an array. This should cause
    // rollback to fail, since applyOps expects an array of ops.
    auto applyOpsCmdOp =
        makeCommandOp(Timestamp(2, 2),
                      boost::none,
                      NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin, "$cmd"),
                      BSON("applyOps" << BSON("not" << "array")),
                      2);

    auto status = rollbackOps({applyOpsCmdOp});
    ASSERT_NOT_OK(status);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsNamespaceAndUUIDsOfSingleOplogEntry) {
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    const auto insertOp = _insertDocAndReturnOplogEntry(BSON("_id" << 1), uuid, nss, 2);

    ASSERT_OK(rollbackOps({insertOp}));
    std::set<NamespaceString> expectedNamespaces = {nss};
    std::set<UUID> expectedUUIDs = {uuid};
    ASSERT(expectedNamespaces == _rbInfo.rollbackNamespaces);
    ASSERT(expectedUUIDs == _rbInfo.rollbackUUIDs);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsMultipleNamespacesAndUUIDsOfOplogEntries) {
    const auto makeInsertOp = [&](UUID uuid, NamespaceString nss, Timestamp ts, int recordId) {
        return _insertDocAndReturnOplogEntry(BSON("_id" << 1), uuid, nss, recordId);
    };

    const auto nss1 = NamespaceString::createNamespaceString_forTest("test", "coll1");
    const auto nss2 = NamespaceString::createNamespaceString_forTest("test", "coll2");
    const auto nss3 = NamespaceString::createNamespaceString_forTest("test", "coll3");

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
    std::set<UUID> expectedUUIDs = {uuid1, uuid2, uuid3};
    ASSERT(expectedNamespaces == _rbInfo.rollbackNamespaces);
    ASSERT(expectedUUIDs == _rbInfo.rollbackUUIDs);
}

TEST_F(RollbackImplObserverInfoTest, RollbackFailsOnUnknownOplogEntryCommandType) {
    // Create a command of an unknown type.
    auto unknownCmdOp =
        makeCommandOp(Timestamp(2, 2),
                      boost::none,
                      NamespaceString::createNamespaceString_forTest(DatabaseName::kAdmin, "$cmd"),
                      BSON("unknownCommand" << 1),
                      2);

    auto commonOp = makeOpAndRecordId(1);
    _remoteOplog->setOperations({commonOp});
    ASSERT_OK(_insertOplogEntry(commonOp.first));
    ASSERT_OK(_insertOplogEntry(unknownCmdOp.first));

    const StringData err(
        "Enumeration value 'unknownCommand' for field 'commandString' is not a valid value.");
    ASSERT_THROWS_CODE_AND_WHAT(
        _rollback->runRollback(_opCtx.get()), DBException, ErrorCodes::BadValue, err);
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
    const auto nss = NamespaceString::createNamespaceString_forTest("test", "coll");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);

    const auto insertOp = _insertDocAndReturnOplogEntry(BSON("_id" << 1), uuid, nss, 2);
    ASSERT_OK(rollbackOps({insertOp}));
    ASSERT(_rbInfo.rollbackSessionIds.empty());
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
                               redactTenant(nss),
                               BSON("_id" << "not_the_shard_id_document"),
                               boost::none,
                               2);
    ASSERT_OK(rollbackOps({deleteOp}));
    ASSERT_FALSE(_rbInfo.shardIdentityRolledBack);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsConfigVersionRollback) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    const auto uuid = UUID::gen();
    const auto nss = NamespaceString::kConfigVersionNamespace;
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    auto insertOp = makeCRUDOp(OpTypeEnum::kInsert,
                               Timestamp(2, 2),
                               uuid,
                               nss.ns_forTest(),
                               BSON("_id" << "a"),
                               boost::none,
                               2);

    ASSERT_OK(rollbackOps({insertOp}));
    ASSERT(_rbInfo.configServerConfigVersionRolledBack);
}

TEST_F(RollbackImplObserverInfoTest, RollbackDoesntRecordConfigVersionRollbackForShardServer) {
    serverGlobalParams.clusterRole = ClusterRole::ShardServer;
    const auto uuid = UUID::gen();
    const auto nss = NamespaceString::kConfigVersionNamespace;
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    auto insertOp = makeCRUDOp(OpTypeEnum::kInsert,
                               Timestamp(2, 2),
                               uuid,
                               nss.ns_forTest(),
                               BSON("_id" << "a"),
                               boost::none,
                               2);
    ASSERT_OK(rollbackOps({insertOp}));
    ASSERT_FALSE(_rbInfo.configServerConfigVersionRolledBack);
}

TEST_F(RollbackImplObserverInfoTest, RollbackDoesntRecordConfigVersionRollbackForNonInsert) {
    serverGlobalParams.clusterRole = {ClusterRole::ShardServer, ClusterRole::ConfigServer};
    const auto uuid = UUID::gen();
    const auto nss = NamespaceString::kConfigVersionNamespace;
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    auto deleteOp = makeCRUDOp(OpTypeEnum::kDelete,
                               Timestamp(2, 2),
                               uuid,
                               nss.ns_forTest(),
                               BSON("_id" << "a"),
                               boost::none,
                               2);
    ASSERT_OK(rollbackOps({deleteOp}));
    ASSERT_FALSE(_rbInfo.configServerConfigVersionRolledBack);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsInsertOpsInUUIDToIdMap) {
    const auto nss1 = NamespaceString::createNamespaceString_forTest("db.people");
    const auto uuid1 = UUID::gen();
    const auto coll1 = _initializeCollection(_opCtx.get(), uuid1, nss1);
    const auto obj1 = BSON("_id" << "kyle");
    const auto insertOp1 = _insertDocAndReturnOplogEntry(obj1, uuid1, nss1, 2);

    const auto nss2 = NamespaceString::createNamespaceString_forTest("db.persons");
    const auto uuid2 = UUID::gen();
    const auto coll2 = _initializeCollection(_opCtx.get(), uuid2, nss2);
    const auto obj2 = BSON("_id" << "jungsoo");
    const auto insertOp2 = _insertDocAndReturnOplogEntry(obj2, uuid2, nss2, 3);

    ASSERT_OK(rollbackOps({insertOp2, insertOp1}));
    ASSERT_EQ(_rbInfo.rollbackDeletedIdsMap.size(), 2UL);

    assertRollbackInfoContainsObjectForUUID(uuid1, obj1);
    assertRollbackInfoContainsObjectForUUID(uuid2, obj2);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsUpdateOpsInUUIDToIdMap) {
    const auto nss1 = NamespaceString::createNamespaceString_forTest("db.coll1");
    const auto uuid1 = UUID::gen();
    const auto coll1 = _initializeCollection(_opCtx.get(), uuid1, nss1);
    const auto id1 = BSON("_id" << 1);
    const auto updateOp1 = makeCRUDOp(OpTypeEnum::kUpdate,
                                      Timestamp(2, 2),
                                      uuid1,
                                      nss1.ns_forTest(),
                                      BSON("$set" << BSON("foo" << 1)),
                                      id1,
                                      2);

    const auto nss2 = NamespaceString::createNamespaceString_forTest("db.coll2");
    const auto uuid2 = UUID::gen();
    const auto coll2 = _initializeCollection(_opCtx.get(), uuid2, nss2);
    const auto id2 = BSON("_id" << 2);
    const auto updateOp2 = makeCRUDOp(OpTypeEnum::kUpdate,
                                      Timestamp(3, 3),
                                      uuid2,
                                      nss2.ns_forTest(),
                                      BSON("$set" << BSON("foo" << 1)),
                                      id2,
                                      3);

    ASSERT_OK(rollbackOps({updateOp2, updateOp1}));
    ASSERT_EQ(_rbInfo.rollbackDeletedIdsMap.size(), 2UL);

    assertRollbackInfoContainsObjectForUUID(uuid1, id1);
    assertRollbackInfoContainsObjectForUUID(uuid2, id2);
}

TEST_F(RollbackImplObserverInfoTest, RollbackRecordsMultipleInsertOpsForSameNamespace) {
    const auto nss = NamespaceString::createNamespaceString_forTest("db.coll");
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
    const auto nss = NamespaceString::createNamespaceString_forTest("db.coll");
    const auto uuid = UUID::gen();
    const auto coll = _initializeCollection(_opCtx.get(), uuid, nss);
    const auto obj1 = BSON("_id" << 1);
    const auto updateOp1 = makeCRUDOp(OpTypeEnum::kUpdate,
                                      Timestamp(2, 2),
                                      uuid,
                                      nss.ns_forTest(),
                                      BSON("$set" << BSON("foo" << 1)),
                                      obj1,
                                      2);

    const auto obj2 = BSON("_id" << 2);
    const auto updateOp2 = makeCRUDOp(OpTypeEnum::kUpdate,
                                      Timestamp(3, 3),
                                      uuid,
                                      nss.ns_forTest(),
                                      BSON("$set" << BSON("bar" << 2)),
                                      obj2,
                                      3);

    ASSERT_OK(rollbackOps({updateOp2, updateOp1}));
    ASSERT_EQ(_rbInfo.rollbackDeletedIdsMap.size(), 1UL);

    assertRollbackInfoContainsObjectForUUID(uuid, obj1);
    assertRollbackInfoContainsObjectForUUID(uuid, obj2);
}

}  // namespace
}  // namespace repl
}  // namespace mongo
