/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_direct_crud.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo::repl {
namespace {

/**
 * Simple wrappers around storage engine direct write functions that take care of retrieving
 * necessary inputs.
 */
StatusWith<UniqueBuffer> _get(OperationContext* opCtx,
                              StringData ident,
                              std::span<const char> key) {
    auto* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    auto* ru = shard_role_details::getRecoveryUnit(opCtx);
    return storage_engine_direct_crud::get(*storageEngine, *ru, ident, key);
}

StatusWith<UniqueBuffer> _get(OperationContext* opCtx, StringData ident, int64_t key) {
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
                                       StringData ident,
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

TEST_F(ApplyContainerOpsTest, ContainerOpsApplyOpsTimestampVisibility) {
    const auto commitTs = Timestamp(20, 1);
    const auto earlierTs = Timestamp(10, 1);

    const int64_t k = 1;
    const auto v = BSONBinData("A", 1, BinDataGeneral);
    auto entry = makeContainerInsertOplogEntry(OpTime(), _intIdent, k, v);

    // Having an applyOps timestamp and non-replicated writes matches the conditions of secondary
    // application of container ops in a wrapping apply ops
    entry.setApplyOpsTimestamp(commitTs);
    {
        repl::UnreplicatedWritesBlock uwb(_opCtx.get());
        ASSERT_OK(applyContainerOpHelper(_opCtx.get(), entry));
    }

    auto* ru = shard_role_details::getRecoveryUnit(_opCtx.get());

    // A read prior to the apply ops' timestamp should not see the write.
    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kProvided, earlierTs);
    auto missing = _get(_opCtx.get(), _intIdent, k);
    ASSERT_EQ(missing.getStatus(), ErrorCodes::NoSuchKey);

    // A read at or after the apply ops' timestamp should see the write.
    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kProvided, commitTs);
    auto visible = _get(_opCtx.get(), _intIdent, k);
    ASSERT_OK(visible.getStatus());
    ASSERT_EQ(0, std::memcmp(visible.getValue().get(), v.data, v.length));

    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kNoTimestamp);
}

TEST_F(ApplyContainerOpsTest, GroupedContainerOpsApplyOpsTimestampVisibility) {
    const auto commitTs = Timestamp(20, 1);
    const auto earlierTs = Timestamp(10, 1);

    const int64_t k1 = 1;
    const int64_t k2 = 2;
    const auto v1 = BSONBinData("A", 1, BinDataGeneral);
    const auto v2 = BSONBinData("B", 1, BinDataGeneral);

    auto insert1 = makeContainerInsertOplogEntry(OpTime(), _intIdent, k1, v1);
    auto insert2 = makeContainerInsertOplogEntry(OpTime(), _intIdent, k2, v2);
    auto delete1 = makeContainerDeleteOplogEntry(OpTime(), _intIdent, k1);
    insert1.setApplyOpsTimestamp(commitTs);
    insert2.setApplyOpsTimestamp(commitTs);
    delete1.setApplyOpsTimestamp(commitTs);

    {
        repl::UnreplicatedWritesBlock uwb(_opCtx.get());
        std::vector<ApplierOperation> ops{{&insert1}, {&insert2}, {&delete1}};
        ASSERT_OK(applyContainerOperations(_opCtx.get(), ops, OplogApplication::Mode::kSecondary));
    }

    auto* ru = shard_role_details::getRecoveryUnit(_opCtx.get());

    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kProvided, earlierTs);
    ASSERT_EQ(_get(_opCtx.get(), _intIdent, k1).getStatus(), ErrorCodes::NoSuchKey);
    ASSERT_EQ(_get(_opCtx.get(), _intIdent, k2).getStatus(), ErrorCodes::NoSuchKey);

    ru->abandonSnapshot();
    ru->setTimestampReadSource(mongo::RecoveryUnit::ReadSource::kProvided, commitTs);
    ASSERT_EQ(_get(_opCtx.get(), _intIdent, k1).getStatus(), ErrorCodes::NoSuchKey);
    auto visible = _get(_opCtx.get(), _intIdent, k2);
    ASSERT_OK(visible.getStatus());
    ASSERT_EQ(0, std::memcmp(visible.getValue().get(), v2.data, v2.length));

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

    auto insert1 =
        makeContainerInsertOplogEntry(OpTime(), _intIdent, 1, BSONBinData("A", 1, BinDataGeneral));
    auto insert2 =
        makeContainerInsertOplogEntry(OpTime(), _intIdent, 2, BSONBinData("B", 1, BinDataGeneral));
    insert1.setApplyOpsTimestamp(ts1);
    insert2.setApplyOpsTimestamp(ts2);

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

    auto insert1 = makeContainerInsertOplogEntry(OpTime(), _intIdent, k1, v1);
    auto updateMissing = makeContainerUpdateOplogEntry(OpTime(), _intIdent, kMissing, v1);
    insert1.setApplyOpsTimestamp(commitTs);
    updateMissing.setApplyOpsTimestamp(commitTs);

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

TEST_F(ApplyContainerOpsTest, ParseContainerUpdateFormatFailures) {
    int64_t k = 1;
    auto v = BSONBinData("V", 1, BinDataGeneral);
    auto wrongTypeV = "notBinData";
    auto version = static_cast<int64_t>(container::UpdateOplogEntryVersion::kFullReplacementV1);

    auto base = [&]() {
        return makeBaseParams(_nss,
                              _intIdent,
                              OpTypeEnum::kContainerUpdate,
                              BSON("k" << k << "v" << v << "$v" << version));
    };

    // missing container
    {
        auto p = base();
        p.container = boost::none;
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704701);
    }
    // missing key
    {
        auto p = base();
        p.oField = BSON("v" << v << "$v" << 1);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::IDLFailedToParse);
    }
    // missing $v
    {
        auto p = base();
        p.oField = BSON("k" << k << "v" << v);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::IDLFailedToParse);
    }
    // $v must be numeric
    {
        auto p = base();
        p.oField = BSON("k" << k << "v" << v << "$v" << "notANumber");
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::TypeMismatch);
    }
    // missing value
    {
        auto p = base();
        p.oField = BSON("k" << k << "$v" << 1);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::IDLFailedToParse);
    }
    // value type must be binData
    {
        auto p = base();
        p.oField = BSON("k" << k << "v" << wrongTypeV << "$v" << version);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::TypeMismatch);
    }
}

TEST_F(ApplyContainerOpsTest, ParseContainerOpFormatFailures) {
    int64_t k = 1;
    auto v = BSONBinData("V", 1, BinDataGeneral);
    auto wrongTypeV = "notBinData";
    auto wrongTypeK = "notBinDataOrInt64";

    auto base = [&]() {
        return makeBaseParams(
            _nss, _intIdent, OpTypeEnum::kContainerInsert, BSON("k" << k << "v" << v));
    };

    /*
     * Container operations ('ci' and 'cd') must take the following form:
     *
     * {
     *   ...                            // base oplog entry fields
     *   "op": "ci" | "cd",
     *   "container": <string>,
     *   "o": {
     *     "k": <BinData | NumberLong>,
     *     "v": <BinData>               // only allowed for "ci"
     *   }
     * }
     */

    // missing container
    {
        auto p = base();
        p.container = boost::none;
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704701);
    }
    // missing key
    {
        auto p = base();
        p.oField = BSON("v" << v);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::IDLFailedToParse);
    }
    // missing value
    {
        auto p = base();
        p.oField = BSON("k" << k);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::IDLFailedToParse);
    }
    // container delete cannot contain value
    {
        auto p = base();
        p.opType = OpTypeEnum::kContainerDelete;
        p.oField = BSON("k" << k << "v" << v);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704704);
    }
    // key type must be binData or numberLong
    {
        auto p = base();
        p.oField = BSON("k" << wrongTypeK << "v" << v);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 12270900);
    }
    // value type must be binData
    {
        auto p = base();
        p.oField = BSON("k" << k << "v" << wrongTypeV);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, ErrorCodes::TypeMismatch);
    }
}

}  // namespace
}  // namespace mongo::repl
