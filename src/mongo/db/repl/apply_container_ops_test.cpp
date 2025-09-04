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

#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/oplog_applier_impl_test_fixture.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/storage/kv/kv_engine.h"
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
 * Helper for applyContainerOperation_inlock that satisfies its preconditions.
 */
Status applyContainerOpHelper(OperationContext* opCtx,
                              const OplogEntry& e,
                              LockMode collMode = MODE_IX) {
    const auto nss = e.getNss();

    boost::optional<AutoGetCollection> coll;
    if (collMode != MODE_NONE) {
        coll.emplace(opCtx, nss, collMode);
    }
    return applyContainerOperation_inlock(opCtx, {&e}, OplogApplication::Mode::kSecondary);
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
        auto* se = getServiceContext()->getStorageEngine();
        _bytesIdent = se->generateNewInternalIdent();
        _intIdent = se->generateNewInternalIdent();
        auto ru = se->newRecoveryUnit();
        _trsBytes = se->getEngine()->makeTemporaryRecordStore(*ru, _bytesIdent, KeyFormat::String);
        _trsInt = se->getEngine()->makeTemporaryRecordStore(*ru, _intIdent, KeyFormat::Long);
        _nss = NamespaceString::createNamespaceString_forTest("test.t");
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
        return makeContainerInsertOplogEntry(OpTime(), _nss, _bytesIdent, k, v);
    };

    auto makeDelete = [&](BSONBinData k) {
        return makeContainerDeleteOplogEntry(OpTime(), _nss, _bytesIdent, k);
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

TEST_F(ApplyContainerOpsTest, ContainerOpApplyIntKey) {
    int64_t k1 = 1, k2 = 2, k3 = 3, k4 = 4;
    auto v1 = BSONBinData("A", 1, BinDataGeneral);
    auto v2 = BSONBinData("B", 1, BinDataGeneral);
    auto v3 = BSONBinData("C", 1, BinDataGeneral);
    auto v4 = BSONBinData("D", 1, BinDataGeneral);

    auto makeInsert = [&](int64_t k, BSONBinData v) {
        return makeContainerInsertOplogEntry(OpTime(), _nss, _intIdent, k, v);
    };
    auto makeDelete = [&](int64_t k) {
        return makeContainerDeleteOplogEntry(OpTime(), _nss, _intIdent, k);
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


TEST_F(ApplyContainerOpsTest, ApplyContainerOpInvalidOpType) {
    auto k = 1;
    auto v = BSONBinData("V", 1, BinDataGeneral);

    auto params = makeBaseParams(_nss, _bytesIdent, OpTypeEnum::kNoop, BSON("k" << k << "v" << v));
    auto op = DurableOplogEntry(params);

    ASSERT_THROWS_CODE(applyContainerOpHelper(_opCtx.get(), {op}), DBException, 10704705);
}

DEATH_TEST_F(ApplyContainerOpsTest, ApplyContainerOpRequiresIXLock, "invariant") {
    int64_t k = 1;
    auto v = BSONBinData("V", 1, BinDataGeneral);

    OplogEntry op = {DurableOplogEntry{
        makeBaseParams(_nss, _intIdent, OpTypeEnum::kContainerInsert, BSON("k" << k << "v" << v))}};

    auto status = applyContainerOpHelper(_opCtx.get(), op, MODE_NONE);
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
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704702);
    }
    // missing value
    {
        auto p = base();
        p.oField = BSON("k" << k);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704703);
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
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704706);
    }
    // value type must be binData
    {
        auto p = base();
        p.oField = BSON("k" << k << "v" << wrongTypeV);
        ASSERT_THROWS_CODE(DurableOplogEntry(p), DBException, 10704707);
    }
}

}  // namespace
}  // namespace mongo::repl
