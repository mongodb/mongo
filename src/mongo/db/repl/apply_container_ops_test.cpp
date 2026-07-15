// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/error_codes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/container_oplog_entry_gen.h"
#include "mongo/db/repl/container_oplog_entry_serialization.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/repl/transaction_oplog_application.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_direct_crud.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <string_view>

namespace mongo::repl {
namespace {

/**
 * Simple wrappers around storage engine direct write functions that take care of retrieving
 * necessary inputs.
 */
StatusWith<UniqueBuffer> _get(OperationContext* opCtx,
                              std::string_view ident,
                              std::span<const char> key) {
    auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto* ru = shard_role_details::getRecoveryUnit(opCtx);
    return storage_engine_direct_crud::get(*storageEngine, *ru, ident, key);
}

StatusWith<UniqueBuffer> _get(OperationContext* opCtx, std::string_view ident, int64_t key) {
    auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto* ru = shard_role_details::getRecoveryUnit(opCtx);
    return storage_engine_direct_crud::get(*storageEngine, *ru, ident, key);
}

/**
 * Applies a single container oplog entry as a secondary.
 */
Status applyContainerOpHelper(OperationContext* opCtx, const OplogEntry& e) {
    auto op = ApplierOperation{&e};
    return applyContainerOperations(
        opCtx, std::span<const ApplierOperation>{&op, 1}, OplogApplication::Mode::kSecondary);
}

DurableOplogEntryParams makeBaseParams(const NamespaceString& nss,
                                       std::string_view ident,
                                       OpTypeEnum type,
                                       const BSONObj& o) {
    return DurableOplogEntryParams{
        .opTime = OpTime(),
        .opType = type,
        .nss = nss,
        .container = ident,
        .oField = o,
        .wallClockTime = Date_t::now(),
    };
}

// Builds a container-insert ReplOperation suitable for nesting inside an applyOps entry.
ReplOperation makeContainerInsertReplOp(std::string_view ident, int64_t key, BSONBinData value) {
    ContainerInsertOplogEntryO o;
    o.setKey(ContainerKey(key));
    o.setValue(ContainerVal(std::span<const char>{static_cast<const char*>(value.data),
                                                  static_cast<size_t>(value.length)}));
    ReplOperation op;
    op.setOpType(OpTypeEnum::kContainerInsert);
    op.setNss(NamespaceString::kContainerNamespace);
    op.setContainer(ident);
    op.setObject(o.toBSON());
    return op;
}

class ApplyContainerOpsTest : public ServiceContextMongoDTest {
protected:
    void setUp() override {
        ServiceContextMongoDTest::setUp();

        _opCtx = cc().makeOperationContext();
        auto* service = getServiceContext();
        ReplicationCoordinator::set(service, std::make_unique<ReplicationCoordinatorMock>(service));

        auto replCoord = ReplicationCoordinator::get(_opCtx.get());
        ASSERT_OK(replCoord->setFollowerMode(MemberState::RS_SECONDARY));

        auto* se = service->getStorageEngine();
        _bytesIdent = se->generateNewInternalIdent();
        _intIdent = se->generateNewInternalIdent();
        auto ru = se->newRecoveryUnit();
        StorageWriteTransaction swt(*ru);
        _trsBytes = se->getEngine()->makeInternalRecordStore(*ru, _bytesIdent, KeyFormat::String);
        _trsInt = se->getEngine()->makeInternalRecordStore(*ru, _intIdent, KeyFormat::Long);
        swt.commit();
        _nss = NamespaceString::kContainerNamespace;
    }

    ServiceContext::UniqueOperationContext _opCtx;
    std::string _intIdent;
    std::string _bytesIdent;
    std::unique_ptr<RecordStore> _trsBytes;
    std::unique_ptr<RecordStore> _trsInt;
    NamespaceString _nss;
};

TEST_F(ApplyContainerOpsTest, ContainerOpApplyByteKey) {
    const char k1[] = "K1", k2[] = "K2", k3[] = "K3", k4[] = "K4";
    auto v1 = BSONBinData("A", 1, BinDataGeneral);
    auto v2 = BSONBinData("B", 1, BinDataGeneral);
    auto v3 = BSONBinData("C", 1, BinDataGeneral);
    auto v4 = BSONBinData("D", 1, BinDataGeneral);

    auto makeInsert = [&](BSONBinData k, BSONBinData v) {
        return makeContainerInsertOplogEntry(OpTime(), _bytesIdent, k, v);
    };

    auto makeDelete = [&](BSONBinData k) {
        return makeContainerDeleteOplogEntry(OpTime(), _bytesIdent, k);
    };

    auto makeGet = [&](auto k) {
        return _get(_opCtx.get(), _bytesIdent, std::span<const char>(k, strlen(k)));
    };

    // Insert
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert({k1, 2, BinDataGeneral}, v1)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert({k2, 2, BinDataGeneral}, v2)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert({k3, 2, BinDataGeneral}, v3)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert({k4, 2, BinDataGeneral}, v4)));

    // Read and check effects
    auto g1 = makeGet(k1);
    auto g2 = makeGet(k2);
    auto g3 = makeGet(k3);
    auto g4 = makeGet(k4);
    ASSERT_OK(g1.getStatus());
    ASSERT_OK(g2.getStatus());
    ASSERT_OK(g3.getStatus());
    ASSERT_OK(g4.getStatus());
    ASSERT_EQ(0, std::memcmp(g1.getValue().get(), v1.data, v1.length));
    ASSERT_EQ(0, std::memcmp(g2.getValue().get(), v2.data, v2.length));
    ASSERT_EQ(0, std::memcmp(g3.getValue().get(), v3.data, v3.length));
    ASSERT_EQ(0, std::memcmp(g4.getValue().get(), v4.data, v4.length));

    // Delete and check effects
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeDelete({k2, 2, BinDataGeneral})));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeDelete({k4, 2, BinDataGeneral})));

    ASSERT_EQ(makeGet(k2).getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_EQ(makeGet(k4).getStatus(), ErrorCodes::NoSuchKey);

    g1 = makeGet(k1);
    g3 = makeGet(k3);
    ASSERT_OK(g1.getStatus());
    ASSERT_OK(g3.getStatus());
    ASSERT_EQ(0, std::memcmp(g1.getValue().get(), v1.data, v1.length));
    ASSERT_EQ(0, std::memcmp(g3.getValue().get(), v3.data, v3.length));
}

TEST_F(ApplyContainerOpsTest, ContainerOpUpdateByteKey) {
    const char k1[] = "K1", k2[] = "K2";
    auto v1 = BSONBinData("A", 1, BinDataGeneral);
    auto v2 = BSONBinData("B", 1, BinDataGeneral);
    auto v1New = BSONBinData("X", 1, BinDataGeneral);
    auto v2New = BSONBinData("Y", 1, BinDataGeneral);

    auto makeInsert = [&](BSONBinData k, BSONBinData v) {
        return makeContainerInsertOplogEntry(OpTime(), _bytesIdent, k, v);
    };
    auto makeUpdate = [&](BSONBinData k, BSONBinData v) {
        return makeContainerUpdateOplogEntry(OpTime(), _bytesIdent, k, v);
    };
    auto makeGet = [&](auto k) {
        return _get(_opCtx.get(), _bytesIdent, std::span<const char>(k, strlen(k)));
    };

    // Insert initial values
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert({k1, 2, BinDataGeneral}, v1)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert({k2, 2, BinDataGeneral}, v2)));

    // Update values
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeUpdate({k1, 2, BinDataGeneral}, v1New)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeUpdate({k2, 2, BinDataGeneral}, v2New)));

    // Read and verify updated values
    auto g1 = makeGet(k1);
    auto g2 = makeGet(k2);
    ASSERT_OK(g1.getStatus());
    ASSERT_OK(g2.getStatus());
    ASSERT_EQ(0, std::memcmp(g1.getValue().get(), v1New.data, v1New.length));
    ASSERT_EQ(0, std::memcmp(g2.getValue().get(), v2New.data, v2New.length));
}

TEST_F(ApplyContainerOpsTest, ContainerOpUpdateIntKey) {
    int64_t k1 = 1, k2 = 2;
    auto v1 = BSONBinData("A", 1, BinDataGeneral);
    auto v2 = BSONBinData("B", 1, BinDataGeneral);
    auto v1New = BSONBinData("X", 1, BinDataGeneral);
    auto v2New = BSONBinData("Y", 1, BinDataGeneral);

    auto makeInsert = [&](int64_t k, BSONBinData v) {
        return makeContainerInsertOplogEntry(OpTime(), _intIdent, k, v);
    };
    auto makeUpdate = [&](int64_t k, BSONBinData v) {
        return makeContainerUpdateOplogEntry(OpTime(), _intIdent, k, v);
    };
    auto makeGet = [&](int64_t k) {
        return _get(_opCtx.get(), _intIdent, k);
    };

    // Insert initial values
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert(k1, v1)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert(k2, v2)));

    // Update values
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeUpdate(k1, v1New)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeUpdate(k2, v2New)));

    // Read and verify updated values
    auto g1 = makeGet(k1);
    auto g2 = makeGet(k2);
    ASSERT_OK(g1.getStatus());
    ASSERT_OK(g2.getStatus());
    ASSERT_EQ(0, std::memcmp(g1.getValue().get(), v1New.data, v1New.length));
    ASSERT_EQ(0, std::memcmp(g2.getValue().get(), v2New.data, v2New.length));
}

TEST_F(ApplyContainerOpsTest, ContainerOpUpdateNonexistentKeyFails) {
    int64_t kInserted = 1;
    int64_t kMissing = 2;
    auto v = BSONBinData("A", 1, BinDataGeneral);

    // Insert a key so the container is non-empty.
    ASSERT_OK(applyContainerOpHelper(
        _opCtx.get(), makeContainerInsertOplogEntry(OpTime(), _intIdent, kInserted, v)));

    // Updating a different, non-existent key should fail.
    auto entry = makeContainerUpdateOplogEntry(OpTime(), _intIdent, kMissing, v);
    auto status = applyContainerOpHelper(_opCtx.get(), entry);
    ASSERT_EQ(status.code(), ErrorCodes::NoSuchKey);
}

TEST_F(ApplyContainerOpsTest, ContainerOpUpdateOplogVersion) {
    // Container update oplog entries should have version 1.
    int64_t k = 1;
    auto v = BSONBinData("V", 1, BinDataGeneral);
    auto entry = makeContainerUpdateOplogEntry(OpTime(), _intIdent, k, v);
    ASSERT_EQ(entry.getObject()["$v"].safeNumberInt(),
              static_cast<int64_t>(container::UpdateOplogEntryVersion::kFullReplacementV1));
}

TEST_F(ApplyContainerOpsTest, ContainerOpApplyIntKey) {
    int64_t k1 = 1, k2 = 2, k3 = 3, k4 = 4;
    auto v1 = BSONBinData("A", 1, BinDataGeneral);
    auto v2 = BSONBinData("B", 1, BinDataGeneral);
    auto v3 = BSONBinData("C", 1, BinDataGeneral);
    auto v4 = BSONBinData("D", 1, BinDataGeneral);

    auto makeInsert = [&](int64_t k, BSONBinData v) {
        return makeContainerInsertOplogEntry(OpTime(), _intIdent, k, v);
    };
    auto makeDelete = [&](int64_t k) {
        return makeContainerDeleteOplogEntry(OpTime(), _intIdent, k);
    };
    auto makeGet = [&](int64_t k) {
        return _get(_opCtx.get(), _intIdent, k);
    };

    // Insert
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert(k1, v1)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert(k2, v2)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert(k3, v3)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeInsert(k4, v4)));

    // Read and check effects
    auto g1 = makeGet(k1);
    auto g2 = makeGet(k2);
    auto g3 = makeGet(k3);
    auto g4 = makeGet(k4);
    ASSERT_OK(g1.getStatus());
    ASSERT_OK(g2.getStatus());
    ASSERT_OK(g3.getStatus());
    ASSERT_OK(g4.getStatus());
    ASSERT_EQ(0, std::memcmp(g1.getValue().get(), v1.data, v1.length));
    ASSERT_EQ(0, std::memcmp(g2.getValue().get(), v2.data, v2.length));
    ASSERT_EQ(0, std::memcmp(g3.getValue().get(), v3.data, v3.length));
    ASSERT_EQ(0, std::memcmp(g4.getValue().get(), v4.data, v4.length));

    // Delete and check effects
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeDelete(k2)));
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), makeDelete(k4)));

    ASSERT_EQ(makeGet(k2).getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_EQ(makeGet(k4).getStatus(), ErrorCodes::NoSuchKey);

    g1 = makeGet(k1);
    g3 = makeGet(k3);
    ASSERT_OK(g1.getStatus());
    ASSERT_OK(g3.getStatus());
    ASSERT_EQ(0, std::memcmp(g1.getValue().get(), v1.data, v1.length));
    ASSERT_EQ(0, std::memcmp(g3.getValue().get(), v3.data, v3.length));
}

// Fixture for applying container ops through the full secondary oplog applier path.
class ContainerChainApplyTest : public OplogApplierImplTest {
protected:
    void setUp() override {
        OplogApplierImplTest::setUp();
        auto* se = _opCtx->getServiceContext()->getStorageEngine();
        _intIdent = se->generateNewInternalIdent();
        auto ru = se->newRecoveryUnit();
        StorageWriteTransaction swt(*ru);
        _trsInt = se->getEngine()->makeInternalRecordStore(*ru, _intIdent, KeyFormat::Long);
        swt.commit();
    }

    std::string _intIdent;
    std::unique_ptr<RecordStore> _trsInt;
};

// A multi-entry applyOps chain of container ops applied as a secondary commits all of its ops
// together at the chain's commit (terminal) optime, not at the individual entry optimes.
TEST_F(ContainerChainApplyTest, ChainAppliedThroughApplierCommitsAtCommitOptime) {
    const auto t1 = Timestamp(15, 1);        // first partial entry
    const auto t2 = Timestamp(16, 1);        // second partial entry
    const auto commitTs = Timestamp(20, 1);  // terminal entry optime == commit optime
    const auto earlierTs = Timestamp(10, 1);

    const int64_t k1 = 1, k2 = 2, k3 = 3;
    const auto v1 = BSONBinData("A", 1, BinDataGeneral);
    const auto v2 = BSONBinData("B", 1, BinDataGeneral);
    const auto v3 = BSONBinData("C", 1, BinDataGeneral);
    const auto wall = Date_t::now();

    // Two partial entries linked by prevOpTime, then the terminal entry at the commit optime. Each
    // entry's applyOps array holds one container insert.
    auto partial1 = makeApplyOpsOplogEntry(OpTime(t1, 1),
                                           {makeContainerInsertReplOp(_intIdent, k1, v1)},
                                           {},
                                           wall,
                                           {},
                                           OpTime(),
                                           boost::none,
                                           ApplyOpsType::kPartial);
    auto partial2 = makeApplyOpsOplogEntry(OpTime(t2, 1),
                                           {makeContainerInsertReplOp(_intIdent, k2, v2)},
                                           {},
                                           wall,
                                           {},
                                           OpTime(t1, 1),
                                           boost::none,
                                           ApplyOpsType::kPartial);
    auto terminal = makeApplyOpsOplogEntry(OpTime(commitTs, 1),
                                           {makeContainerInsertReplOp(_intIdent, k3, v3)},
                                           {},
                                           wall,
                                           {},
                                           OpTime(t2, 1),
                                           boost::none,
                                           ApplyOpsType::kTerminal);

    // Production extraction: the partial entries are the in-batch cachedOps, the terminal entry
    // drives the walk. Every op carries the terminal commit optime; getApplyOpsTimestamp() keeps
    // the source entry's optime.
    std::vector<OplogEntry*> cachedOps{&partial1, &partial2};
    auto ops = readTransactionOperationsFromOplogChain(_opCtx.get(), terminal, cachedOps);
    ASSERT_EQ(ops.size(), 3u);
    for (const auto& op : ops) {
        EXPECT_TRUE(op.isContainerOpType());
        EXPECT_EQ(op.getTimestamp(), commitTs);
    }
    ASSERT_TRUE(ops[0].getApplyOpsTimestamp());
    ASSERT_TRUE(ops[1].getApplyOpsTimestamp());
    ASSERT_TRUE(ops[2].getApplyOpsTimestamp());
    EXPECT_EQ(*ops[0].getApplyOpsTimestamp(), t1);
    EXPECT_EQ(*ops[1].getApplyOpsTimestamp(), t2);
    EXPECT_EQ(*ops[2].getApplyOpsTimestamp(), commitTs);

    // Apply through the real per-worker path, which groups the ops and commits them.
    ASSERT_OK(runOpsSteadyState(ops));

    // Nothing is visible before the commit optime -- including at the partial entries' optimes --
    // and all three keys become visible together at the commit optime.
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx.get());
    for (auto readTs : {earlierTs, t1, t2}) {
        ru->abandonSnapshot();
        ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kProvided, readTs);
        EXPECT_EQ(_get(_opCtx.get(), _intIdent, k1).getStatus(), ErrorCodes::NoSuchKey);
        EXPECT_EQ(_get(_opCtx.get(), _intIdent, k2).getStatus(), ErrorCodes::NoSuchKey);
        EXPECT_EQ(_get(_opCtx.get(), _intIdent, k3).getStatus(), ErrorCodes::NoSuchKey);
    }

    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kProvided, commitTs);
    ASSERT_OK(_get(_opCtx.get(), _intIdent, k1).getStatus());
    ASSERT_OK(_get(_opCtx.get(), _intIdent, k2).getStatus());
    ASSERT_OK(_get(_opCtx.get(), _intIdent, k3).getStatus());

    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kNoTimestamp);
}

TEST_F(ApplyContainerOpsTest, ContainerOpsSingleOpTimestampVisibility) {
    const auto commitTs = Timestamp(20, 1);
    const auto earlierTs = Timestamp(10, 1);

    const int64_t k = 1;
    const auto v = BSONBinData("A", 1, BinDataGeneral);
    auto entry = makeContainerInsertOplogEntry(OpTime(commitTs, 0), _intIdent, k, v);

    // Non-replicated writes with a timestamp on the oplog entry matches the conditions of
    // secondary application of a single container op.
    {
        repl::UnreplicatedWritesBlock uwb(_opCtx.get());
        ASSERT_OK(applyContainerOpHelper(_opCtx.get(), entry));
    }

    auto* ru = shard_role_details::getRecoveryUnit(_opCtx.get());

    // A read prior to the oplog entry's timestamp should not see the write.
    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kProvided, earlierTs);
    auto missing = _get(_opCtx.get(), _intIdent, k);
    ASSERT_EQ(missing.getStatus(), ErrorCodes::NoSuchKey);

    // A read at or after the oplog entry's timestamp should see the write.
    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kProvided, commitTs);
    auto visible = _get(_opCtx.get(), _intIdent, k);
    ASSERT_OK(visible.getStatus());
    ASSERT_EQ(0, std::memcmp(visible.getValue().get(), v.data, v.length));

    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kNoTimestamp);
}

// A group of container ops sharing one oplog entry timestamp commits together at that timestamp.
// The delete cancels the first insert, so at the commit timestamp only k2 is visible and nothing
// is visible before it.
TEST_F(ApplyContainerOpsTest, GroupedContainerOpsTimestampVisibility) {
    const auto commitTs = Timestamp(20, 1);
    const auto earlierTs = Timestamp(10, 1);

    const int64_t k1 = 1;
    const int64_t k2 = 2;
    const auto v1 = BSONBinData("A", 1, BinDataGeneral);
    const auto v2 = BSONBinData("B", 1, BinDataGeneral);

    auto insert1 = makeContainerInsertOplogEntry(OpTime(commitTs, 0), _intIdent, k1, v1);
    auto insert2 = makeContainerInsertOplogEntry(OpTime(commitTs, 0), _intIdent, k2, v2);
    auto delete1 = makeContainerDeleteOplogEntry(OpTime(commitTs, 0), _intIdent, k1);

    {
        repl::UnreplicatedWritesBlock uwb(_opCtx.get());
        std::vector<ApplierOperation> ops{{&insert1}, {&insert2}, {&delete1}};
        ASSERT_OK(applyContainerOperations(_opCtx.get(), ops, OplogApplication::Mode::kSecondary));
    }

    auto* ru = shard_role_details::getRecoveryUnit(_opCtx.get());

    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kProvided, earlierTs);
    EXPECT_EQ(_get(_opCtx.get(), _intIdent, k1).getStatus(), ErrorCodes::NoSuchKey);
    EXPECT_EQ(_get(_opCtx.get(), _intIdent, k2).getStatus(), ErrorCodes::NoSuchKey);

    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kProvided, commitTs);
    EXPECT_EQ(_get(_opCtx.get(), _intIdent, k1).getStatus(), ErrorCodes::NoSuchKey);
    auto visible = _get(_opCtx.get(), _intIdent, k2);
    ASSERT_OK(visible.getStatus());
    ASSERT_EQ(0, std::memcmp(visible.getValue().get(), v2.data, v2.length));

    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kNoTimestamp);
}

TEST_F(ApplyContainerOpsTest, ApplyContainerOpInvalidOpType) {
    auto k = 1;
    auto v = BSONBinData("V", 1, BinDataGeneral);

    auto params = makeBaseParams(_nss, _bytesIdent, OpTypeEnum::kNoop, BSON("k" << k << "v" << v));
    auto op = DurableOplogEntry(params);

    ASSERT_THROWS_CODE(applyContainerOpHelper(_opCtx.get(), {op}), DBException, 12337301);
}

TEST_F(ApplyContainerOpsTest, ApplyContainerOpsRejectsEmptySpan) {
    std::vector<ApplierOperation> empty;
    ASSERT_THROWS_CODE(
        applyContainerOperations(_opCtx.get(), empty, OplogApplication::Mode::kSecondary),
        DBException,
        12337300);
}

TEST_F(ApplyContainerOpsTest, ApplyContainerOpsRejectsMismatchedTimestamps) {
    const auto ts1 = Timestamp(10, 1);
    const auto ts2 = Timestamp(20, 1);

    // The shared-commit-timestamp invariant is on getTimestamp(); ops grouped into one
    // applyContainerOperations call must agree on it.
    auto insert1 = makeContainerInsertOplogEntry(
        OpTime(ts1, 0), _intIdent, 1, BSONBinData("A", 1, BinDataGeneral));
    auto insert2 = makeContainerInsertOplogEntry(
        OpTime(ts2, 0), _intIdent, 2, BSONBinData("B", 1, BinDataGeneral));

    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    std::vector<ApplierOperation> ops{{&insert1}, {&insert2}};
    ASSERT_THROWS_CODE(
        applyContainerOperations(_opCtx.get(), ops, OplogApplication::Mode::kSecondary),
        DBException,
        12337302);
}

TEST_F(ApplyContainerOpsTest, GroupedContainerOpsMidBatchFailureRollsBack) {
    const auto commitTs = Timestamp(20, 1);
    const int64_t k1 = 1;
    const int64_t kMissing = 2;
    const auto v1 = BSONBinData("A", 1, BinDataGeneral);

    auto insert1 = makeContainerInsertOplogEntry(OpTime(commitTs, 0), _intIdent, k1, v1);
    auto updateMissing =
        makeContainerUpdateOplogEntry(OpTime(commitTs, 0), _intIdent, kMissing, v1);

    {
        repl::UnreplicatedWritesBlock uwb(_opCtx.get());
        std::vector<ApplierOperation> ops{{&insert1}, {&updateMissing}};
        auto status =
            applyContainerOperations(_opCtx.get(), ops, OplogApplication::Mode::kSecondary);
        ASSERT_EQ(status.code(), ErrorCodes::NoSuchKey);
    }

    ASSERT_EQ(_get(_opCtx.get(), _intIdent, k1).getStatus(), ErrorCodes::NoSuchKey);
}

TEST_F(ApplyContainerOpsTest, ContainerOpsRequiresTimestamp) {
    int64_t k = 1;
    auto v = BSONBinData("V", 1, BinDataGeneral);
    auto nullOpTime = OpTime();

    auto insertEntry = makeContainerInsertOplogEntry(nullOpTime, _bytesIdent, k, v);
    auto deleteEntry = makeContainerDeleteOplogEntry(nullOpTime, _bytesIdent, k);

    repl::UnreplicatedWritesBlock uwb(_opCtx.get());

    ASSERT_THROWS_CODE(applyContainerOpHelper(_opCtx.get(), insertEntry), DBException, 11348300);
    ASSERT_THROWS_CODE(applyContainerOpHelper(_opCtx.get(), deleteEntry), DBException, 11348300);
}

TEST_F(ApplyContainerOpsTest, ContainerOpsRejectMismatchedExistingCommitTimestamp) {
    const Timestamp commitTs(20, 1);
    const Timestamp existingTs(10, 1);
    int64_t k = 1;
    auto v = BSONBinData("V", 1, BinDataGeneral);
    auto insertEntry = makeContainerInsertOplogEntry(OpTime(commitTs, 0), _bytesIdent, k, v);
    auto deleteEntry = makeContainerDeleteOplogEntry(OpTime(commitTs, 0), _bytesIdent, k);

    repl::UnreplicatedWritesBlock uwb(_opCtx.get());
    auto* ru = shard_role_details::getRecoveryUnit(_opCtx.get());
    ru->setCommitTimestamp(existingTs);

    ASSERT_THROWS_CODE(applyContainerOpHelper(_opCtx.get(), insertEntry), DBException, 11348301);
    ASSERT_THROWS_CODE(applyContainerOpHelper(_opCtx.get(), deleteEntry), DBException, 11348301);

    ru->clearCommitTimestamp();
}

/*
 * Container operations ('ci', 'cd', and 'cu') must take the following form:
 *
 * {
 *   ...                            // base oplog entry fields
 *   "op": "ci" | "cd" | "cu",
 *   "container": <string>,
 *   "o": {
 *     "k": <BinData | NumberLong>,
 *     "v": <BinData>               // only allowed for "ci" and "cu"
 *     "$v": <NumberLong>           // only allowed for "cu"
 *   }
 * }
 */
class ApplyContainerOpsParseContainerFormatFailuresTest : public ApplyContainerOpsTest {
public:
    ApplyContainerOpsParseContainerFormatFailuresTest() : _v("V", 1, BinDataGeneral) {}

protected:
    DurableOplogEntryParams BaseInsert() {
        return makeBaseParams(
            _nss, _intIdent, OpTypeEnum::kContainerInsert, BSON("k" << _k << "v" << _v));
    }

    DurableOplogEntryParams BaseDelete() {
        return makeBaseParams(_nss, _intIdent, OpTypeEnum::kContainerDelete, BSON("k" << _k));
    }

    DurableOplogEntryParams BaseUpdate() {
        return makeBaseParams(_nss,
                              _intIdent,
                              OpTypeEnum::kContainerUpdate,
                              BSON("k" << _k << "v" << _v << "$v" << _version));
    }

    static constexpr int64_t _k = 1;
    BSONBinData _v;
    static constexpr auto _wrongTypeV = "notBinData";
    static constexpr auto _wrongTypeK = "notBinDataOrInt64";
    static constexpr auto _version =
        static_cast<int64_t>(container::UpdateOplogEntryVersion::kFullReplacementV1);
};

TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, MissingContainerInsert) {
    auto p = BaseInsert();
    p.container = boost::none;
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704701);
}

TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, MissingContainerDelete) {
    auto p = BaseDelete();
    p.container = boost::none;
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704701);
}

TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, MissingContainerUpdate) {
    auto p = BaseUpdate();
    p.container = boost::none;
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704701);
}

// missing key
TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, MissingKeyInsert) {
    auto p = BaseInsert();
    p.oField = BSON("v" << _v);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::IDLFailedToParse);
}

TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, MissingKeyDelete) {
    auto p = BaseDelete();
    p.oField = BSONObj();
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::IDLFailedToParse);
}

TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, MissingKeyUpdate) {
    auto p = BaseUpdate();
    p.oField = BSON("v" << _v << "$v" << _version);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::IDLFailedToParse);
}

// key type must be binData or numberLong
TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, InvalidKeyTypeInsert) {
    auto p = BaseInsert();
    p.oField = BSON("k" << _wrongTypeK << "v" << _v);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 12270900);
}

TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, InvalidKeyTypeDelete) {
    auto p = BaseDelete();
    p.oField = BSON("k" << _wrongTypeK);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 12270900);
}

TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, InvalidKeyTypeUpdate) {
    auto p = BaseUpdate();
    p.oField = BSON("k" << _wrongTypeK << "v" << _v << "$v" << _version);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 12270900);
}

// missing value - an integer key must specify a value
TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, MissingValueInsert) {
    auto p = BaseInsert();
    p.oField = BSON("k" << _k);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 13064100);
}

TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, MissingValueUpdate) {
    auto p = BaseUpdate();
    p.oField = BSON("k" << _k << "$v" << _version);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::IDLFailedToParse);
}

// value type must be binData or array of binData
TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, InvalidValueTypeInsert) {
    auto p = BaseInsert();
    p.oField = BSON("k" << _k << "v" << _wrongTypeV);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::TypeMismatch);
}

TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, InvalidValueTypeUpdate) {
    auto p = BaseUpdate();
    p.oField = BSON("k" << _k << "v" << _wrongTypeV << "$v" << _version);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::TypeMismatch);
}

TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, InvalidArrayTypeUpdate) {
    auto p = BaseUpdate();
    p.oField = BSON("k" << _k << "v" << BSON_ARRAY(BSONBinData()) << "$v" << _version);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 13064101);
}

// delete: cannot contain value
TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, DeleteWithValue) {
    auto p = BaseDelete();
    p.oField = BSON("k" << _k << "v" << _v);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704704);
}

// update: missing version
TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, UpdateMissingVersion) {
    auto p = BaseUpdate();
    p.oField = BSON("k" << _k << "v" << _v);
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::IDLFailedToParse);
}

// update: version must be numeric
TEST_F(ApplyContainerOpsParseContainerFormatFailuresTest, UpdateNonNumericVersion) {
    auto p = BaseUpdate();
    p.oField = BSON("k" << _k << "v" << _v << "$v" << "notANumber");
    ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::TypeMismatch);
}

// Builds a container OplogEntry ('ci'/'cu'/'cd') with a caller-supplied, verbatim 'o' field so that
// the batched (range) encodings can be applied end to end. Constructing the DurableOplogEntry runs
// the same container 'o' validation as production.
OplogEntry makeContainerBatchEntry(const NamespaceString& nss,
                                   std::string_view ident,
                                   OpTypeEnum type,
                                   const BSONObj& o) {
    return OplogEntry(DurableOplogEntry(makeBaseParams(nss, ident, type, o)));
}

// Int-keyed range insert: a NumberLong base key plus an array of values, with the i-th value
// written at key (base + i).
TEST_F(ApplyContainerOpsTest, BatchInsertIntKeyedRange) {
    const int64_t base = 100;
    auto v0 = BSONBinData("A", 1, BinDataGeneral);
    auto v1 = BSONBinData("B", 1, BinDataGeneral);
    auto v2 = BSONBinData("C", 1, BinDataGeneral);

    auto o = BSON("k" << base << "v" << BSON_ARRAY(v0 << v1 << v2));
    auto entry = makeContainerBatchEntry(_nss, _intIdent, OpTypeEnum::kContainerInsert, o);
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), entry));

    auto g0 = _get(_opCtx.get(), _intIdent, base);
    auto g1 = _get(_opCtx.get(), _intIdent, base + 1);
    auto g2 = _get(_opCtx.get(), _intIdent, base + 2);
    ASSERT_OK(g0.getStatus());
    ASSERT_OK(g1.getStatus());
    ASSERT_OK(g2.getStatus());
    EXPECT_EQ(0, std::memcmp(g0.getValue().get(), v0.data, v0.length));
    EXPECT_EQ(0, std::memcmp(g1.getValue().get(), v1.data, v1.length));
    EXPECT_EQ(0, std::memcmp(g2.getValue().get(), v2.data, v2.length));
}

// Int-keyed range insert rolls back entirely if a mid-range write fails.
TEST_F(ApplyContainerOpsTest, BatchInsertIntKeyedRangeMidFailureRollsBack) {
    const int64_t base = 1;
    auto v = BSONBinData("A", 1, BinDataGeneral);

    // Pre-insert 'base + 1' so the second write of the range collides and fails.
    ASSERT_OK(applyContainerOpHelper(
        _opCtx.get(), makeContainerInsertOplogEntry(OpTime(), _intIdent, base + 1, v)));

    auto n0 = BSONBinData("X", 1, BinDataGeneral);
    auto n1 = BSONBinData("Y", 1, BinDataGeneral);
    auto o = BSON("k" << base << "v" << BSON_ARRAY(n0 << n1));
    auto entry = makeContainerBatchEntry(_nss, _intIdent, OpTypeEnum::kContainerInsert, o);
    ASSERT_NOT_OK(applyContainerOpHelper(_opCtx.get(), entry));

    // The first write ('base') was rolled back with the failed op.
    ASSERT_EQ(_get(_opCtx.get(), _intIdent, base).getStatus(), ErrorCodes::NoSuchKey);
}

// Bytes-keyed range insert: an array of BinData keys, each inserted with an empty value.
TEST_F(ApplyContainerOpsTest, BatchInsertBytesKeyedRange) {
    const char k1[] = "K1", k2[] = "K2", k3[] = "K3";

    auto o = BSON("k" << BSON_ARRAY(BSONBinData(k1, 2, BinDataGeneral)
                                    << BSONBinData(k2, 2, BinDataGeneral)
                                    << BSONBinData(k3, 2, BinDataGeneral)));
    auto entry = makeContainerBatchEntry(_nss, _bytesIdent, OpTypeEnum::kContainerInsert, o);
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), entry));

    for (const char* k : {k1, k2, k3}) {
        ASSERT_OK(_get(_opCtx.get(), _bytesIdent, std::span<const char>(k, 2)).getStatus());
    }
}

// An array of keys carries no value. Construction validation permits a value alongside an array
// key, so the apply path must reject it gracefully (a DBException) rather than tripping an
// invariant and crashing the server.
TEST_F(ApplyContainerOpsTest, BatchInsertBytesKeyedRangeWithValueIsRejected) {
    const char k1[] = "K1", k2[] = "K2";
    auto v = BSONBinData("A", 1, BinDataGeneral);

    auto o = BSON(
        "k" << BSON_ARRAY(BSONBinData(k1, 2, BinDataGeneral) << BSONBinData(k2, 2, BinDataGeneral))
            << "v" << v);
    // The entry is constructible (validation 13064100 allows a value with an array key)...
    auto entry = makeContainerBatchEntry(_nss, _bytesIdent, OpTypeEnum::kContainerInsert, o);
    // ...but applying it fails with a graceful error rather than an invariant/fassert.
    ASSERT_THROWS_CODE(applyContainerOpHelper(_opCtx.get(), entry), DBException, 13064104);
}

// Bytes-keyed range delete: an array of BinData keys to remove.
TEST_F(ApplyContainerOpsTest, BatchDeleteBytesKeyedRange) {
    const char k1[] = "K1", k2[] = "K2", k3[] = "K3";
    auto v = BSONBinData("A", 1, BinDataGeneral);
    for (const char* k : {k1, k2, k3}) {
        ASSERT_OK(applyContainerOpHelper(
            _opCtx.get(),
            makeContainerInsertOplogEntry(OpTime(), _bytesIdent, {k, 2, BinDataGeneral}, v)));
    }

    auto o = BSON("k" << BSON_ARRAY(BSONBinData(k1, 2, BinDataGeneral)
                                    << BSONBinData(k3, 2, BinDataGeneral)));
    auto entry = makeContainerBatchEntry(_nss, _bytesIdent, OpTypeEnum::kContainerDelete, o);
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), entry));

    ASSERT_EQ(_get(_opCtx.get(), _bytesIdent, std::span<const char>(k1, 2)).getStatus(),
              ErrorCodes::NoSuchKey);
    ASSERT_OK(_get(_opCtx.get(), _bytesIdent, std::span<const char>(k2, 2)).getStatus());
    ASSERT_EQ(_get(_opCtx.get(), _bytesIdent, std::span<const char>(k3, 2)).getStatus(),
              ErrorCodes::NoSuchKey);
}

// End-to-end within the container layer: build the 'o' field through the container serializer
// (ContainerInsertOplogEntryO::toBSON, which drives ContainerKey/ContainerVal::serialize) rather
// than by hand, then apply it. This exercises the serialize -> parse -> apply round trip -- as
// close to "generate then apply" as is possible until batched-write generation exists.
TEST_F(ApplyContainerOpsTest, BatchInsertIntKeyedRangeSerializedRoundTrip) {
    const int64_t base = 200;
    constexpr std::string_view a = "a", b = "b", c = "c";
    std::vector<std::span<const char>> values{
        {a.data(), a.size()}, {b.data(), b.size()}, {c.data(), c.size()}};

    ContainerInsertOplogEntryO o;
    o.setKey(ContainerKey(base));
    o.setValue(ContainerVal(values));

    auto entry = makeContainerBatchEntry(_nss, _intIdent, OpTypeEnum::kContainerInsert, o.toBSON());
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), entry));

    for (int64_t i = 0; i < 3; ++i) {
        auto g = _get(_opCtx.get(), _intIdent, base + i);
        ASSERT_OK(g.getStatus());
        ASSERT_EQ(0, std::memcmp(g.getValue().get(), values[i].data(), values[i].size()));
    }
}

TEST_F(ApplyContainerOpsTest, BatchInsertBytesKeyedRangeSerializedRoundTrip) {
    constexpr std::string_view k1 = "K1", k2 = "K2", k3 = "K3";
    std::vector<std::span<const char>> keys{
        {k1.data(), k1.size()}, {k2.data(), k2.size()}, {k3.data(), k3.size()}};

    ContainerInsertOplogEntryO o;
    o.setKey(ContainerKey(keys));
    // No value: a bytes-keyed range insert writes empty values.

    auto entry =
        makeContainerBatchEntry(_nss, _bytesIdent, OpTypeEnum::kContainerInsert, o.toBSON());
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), entry));

    for (std::string_view k : {k1, k2, k3}) {
        ASSERT_OK(
            _get(_opCtx.get(), _bytesIdent, std::span<const char>(k.data(), k.size())).getStatus());
    }
}

TEST_F(ApplyContainerOpsTest, BatchDeleteBytesKeyedRangeSerializedRoundTrip) {
    constexpr std::string_view k1 = "K1", k2 = "K2";
    auto v = BSONBinData("A", 1, BinDataGeneral);
    for (std::string_view k : {k1, k2}) {
        ASSERT_OK(applyContainerOpHelper(
            _opCtx.get(),
            makeContainerInsertOplogEntry(
                OpTime(), _bytesIdent, {k.data(), static_cast<int>(k.size()), BinDataGeneral}, v)));
    }

    std::vector<std::span<const char>> keys{{k1.data(), k1.size()}, {k2.data(), k2.size()}};
    ContainerDeleteOplogEntryO o;
    o.setKey(ContainerKey(keys));

    auto entry =
        makeContainerBatchEntry(_nss, _bytesIdent, OpTypeEnum::kContainerDelete, o.toBSON());
    ASSERT_OK(applyContainerOpHelper(_opCtx.get(), entry));

    ASSERT_EQ(
        _get(_opCtx.get(), _bytesIdent, std::span<const char>(k1.data(), k1.size())).getStatus(),
        ErrorCodes::NoSuchKey);
    ASSERT_EQ(
        _get(_opCtx.get(), _bytesIdent, std::span<const char>(k2.data(), k2.size())).getStatus(),
        ErrorCodes::NoSuchKey);
}

}  // namespace
}  // namespace mongo::repl
