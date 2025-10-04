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

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/ident.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_kv_engine.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstring>
#include <memory>
#include <string>

#include <wiredtiger.h>

#include <boost/move/utility_core.hpp>

namespace mongo {
namespace {

TEST(WiredTigerRecordStoreTest, StorageSizeStatisticsDisabled) {
    WiredTigerHarnessHelper harnessHelper("statistics=(none)");
    std::unique_ptr<RecordStore> rs(harnessHelper.newRecordStore("a.b"));

    ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    ASSERT_THROWS(rs->storageSize(ru), AssertionException);
}

TEST(WiredTigerRecordStoreTest, SizeStorer1) {
    WiredTigerHarnessHelper harnessHelper;
    std::string indexUri = WiredTigerUtil::kTableUriPrefix + "myindex";
    WiredTigerSizeStorer ss(&harnessHelper.connection(), indexUri);

    std::unique_ptr<RecordStore> rs(harnessHelper.newRecordStore());
    checked_cast<WiredTigerRecordStore*>(rs.get())->setSizeStorer(&ss);

    std::string ident = std::string{rs->getIdent()};
    std::string uri = std::string{checked_cast<WiredTigerRecordStore*>(rs.get())->getURI()};

    int N = 12;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        StorageWriteTransaction txn(ru);
        for (int i = 0; i < N; i++) {
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 "a",
                                 2,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
        }
        txn.commit();
    }

    ASSERT_EQUALS(N, rs->numRecords());

    rs.reset();

    {
        WiredTigerSession session{&harnessHelper.connection()};
        auto& info = *ss.load(session, uri);
        ASSERT_EQUALS(N, info.numRecords.load());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        WiredTigerRecordStore::Params params;
        params.ident = ident;
        params.engineName = std::string{kWiredTigerEngineName};
        params.keyFormat = KeyFormat::Long;
        params.overwrite = true;
        params.isLogged = false;
        params.forceUpdateWithFullDocument = false;
        params.inMemory = false;
        params.sizeStorer = &ss;
        params.tracksSizeAdjustments = true;

        auto ret = new WiredTigerRecordStore(
            nullptr,
            WiredTigerRecoveryUnit::get(*shard_role_details::getRecoveryUnit(opCtx.get())),
            params);
        rs.reset(ret);
    }

    ASSERT_EQUALS(N, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());

        auto& ru = *checked_cast<WiredTigerRecoveryUnit*>(
            shard_role_details::getRecoveryUnit(opCtx.get()));

        {
            StorageWriteTransaction txn(ru);
            WiredTigerSession* s = ru.getSession();
            invariantWTOK(s->create(indexUri.c_str(), ""), *s);
            txn.commit();
        }

        ss.flush(true);
    }

    {
        WiredTigerSession session{&harnessHelper.connection()};
        WiredTigerSizeStorer ss2(&harnessHelper.connection(), indexUri);
        auto info = ss2.load(session, uri);
        ASSERT_EQUALS(N, info->numRecords.load());
    }
}

class SizeStorerUpdateTest : public mongo::unittest::Test {
private:
    void setUp() override {
        rs = harnessHelper.newRecordStore();
        WiredTigerRecordStore* wtRS = checked_cast<WiredTigerRecordStore*>(rs.get());
        wtRS->setSizeStorer(&sizeStorer);
        ident = std::string{wtRS->getIdent()};
        uri = std::string{wtRS->getURI()};
    }
    void tearDown() override {
        rs.reset();
        sizeStorer.flush(false);
    }

protected:
    long long getNumRecords() {
        WiredTigerSession session{&harnessHelper.connection()};
        return sizeStorer.load(session, uri)->numRecords.load();
    }

    long long getDataSize() {
        WiredTigerSession session{&harnessHelper.connection()};
        return sizeStorer.load(session, uri)->dataSize.load();
    }

    WiredTigerHarnessHelper harnessHelper;
    WiredTigerSizeStorer sizeStorer{&harnessHelper.connection(),
                                    std::string{WiredTigerUtil::kTableUriPrefix} +
                                        ident::kSizeStorer};
    std::unique_ptr<RecordStore> rs;
    std::string ident;
    std::string uri;
};

// Basic validation - size storer data is updated.
TEST_F(SizeStorerUpdateTest, Basic) {
    ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
    long long val = 5;
    rs->updateStatsAfterRepair(val, val);
    ASSERT_EQUALS(getNumRecords(), val);
    ASSERT_EQUALS(getDataSize(), val);
};

TEST_F(SizeStorerUpdateTest, DataSizeModification) {
    ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    RecordId recordId;
    {
        StorageWriteTransaction txn(ru);
        auto rId = rs->insertRecord(opCtx.get(),
                                    *shard_role_details::getRecoveryUnit(opCtx.get()),
                                    "12345",
                                    5,
                                    Timestamp{1});
        ASSERT_TRUE(rId.isOK());
        recordId = rId.getValue();
        txn.commit();
    }

    ASSERT_EQ(getDataSize(), 5);
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->updateRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), recordId, "54321", 5));
        txn.commit();
    }
    ASSERT_EQ(getDataSize(), 5);
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->updateRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), recordId, "1234", 4));
        txn.commit();
    }
    ASSERT_EQ(getDataSize(), 4);

    RecordData oldRecordData("1234", 4);
    {
        StorageWriteTransaction txn(ru);
        const auto damageSource = "";
        DamageVector damageVector;
        damageVector.push_back(DamageEvent(0, 0, 0, 1));
        auto newDoc = rs->updateWithDamages(opCtx.get(),
                                            *shard_role_details::getRecoveryUnit(opCtx.get()),
                                            recordId,
                                            oldRecordData,
                                            damageSource,
                                            damageVector);
        ASSERT_TRUE(newDoc.isOK());
        oldRecordData = newDoc.getValue().getOwned();
        ASSERT_EQ(std::memcmp(oldRecordData.data(), "234", 3), 0);
        ASSERT_EQ(getDataSize(), 3);
        txn.commit();
    }
    {
        StorageWriteTransaction txn(ru);
        const auto damageSource = "3456";
        DamageVector damageVector;
        damageVector.push_back(DamageEvent(0, 4, 1, 2));
        ASSERT_TRUE(rs->updateWithDamages(opCtx.get(),
                                          *shard_role_details::getRecoveryUnit(opCtx.get()),
                                          recordId,
                                          oldRecordData,
                                          damageSource,
                                          damageVector)
                        .isOK());
        ASSERT_EQ(getDataSize(), 5);
        txn.commit();
    }
}

// Verify that the size storer contains accurate data after a transaction rollback just before a
// flush (simulating a shutdown). That is, that the rollback marks the size info as dirty, and is
// properly flushed to disk.
TEST_F(SizeStorerUpdateTest, ReloadAfterRollbackAndFlush) {
    ServiceContext::UniqueOperationContext opCtx(harnessHelper.newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    // Do an op for which the sizeInfo is persisted, for safety so we don't check against 0.
    {
        StorageWriteTransaction txn(ru);
        auto rId = rs->insertRecord(opCtx.get(),
                                    *shard_role_details::getRecoveryUnit(opCtx.get()),
                                    "12345",
                                    5,
                                    Timestamp{1});
        ASSERT_TRUE(rId.isOK());

        txn.commit();
    }

    // An operation to rollback, with a flush between the original modification and the rollback.
    {
        StorageWriteTransaction txn(ru);
        auto rId = rs->insertRecord(opCtx.get(),
                                    *shard_role_details::getRecoveryUnit(opCtx.get()),
                                    "12345",
                                    5,
                                    Timestamp{2});
        ASSERT_TRUE(rId.isOK());

        ASSERT_EQ(getNumRecords(), 2);
        ASSERT_EQ(getDataSize(), 10);
        // Mark size info as clean, before rollback is done.
        sizeStorer.flush(false);
    }

    // Simulate a shutdown and restart, which loads the size storer from disk.
    sizeStorer.flush(true);

    WiredTigerSizeStorer sizeStorer(&harnessHelper.connection(),
                                    std::string{WiredTigerUtil::kTableUriPrefix} +
                                        ident::kSizeStorer);
    WiredTigerSession session{&harnessHelper.connection()};

    // As the operation was rolled back, numRecords and dataSize should be for the first op only. If
    // rollback does not properly mark the sizeInfo as dirty, on load sizeInfo will account for the
    // two operations, as the rollback sizeInfo update has not been flushed.
    ASSERT_EQ(sizeStorer.load(session, uri)->numRecords.load(), 1);
    ASSERT_EQ(sizeStorer.load(session, uri)->dataSize.load(), 5);
};

}  // namespace
}  // namespace mongo
