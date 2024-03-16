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

#include "mongo/db/storage/record_store_test_harness.h"

#include <algorithm>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <cstring>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/bson/oid.h"
#include "mongo/db/catalog/clustered_collection_options_gen.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace {
std::function<std::unique_ptr<RecordStoreHarnessHelper>(RecordStoreHarnessHelper::Options)>
    recordStoreHarnessFactory;
}

void registerRecordStoreHarnessHelperFactory(
    std::function<std::unique_ptr<RecordStoreHarnessHelper>(RecordStoreHarnessHelper::Options)>
        factory) {
    recordStoreHarnessFactory = std::move(factory);
}

auto newRecordStoreHarnessHelper(RecordStoreHarnessHelper::Options options)
    -> std::unique_ptr<RecordStoreHarnessHelper> {
    return recordStoreHarnessFactory(options);
}

namespace {

TEST(RecordStoreTestHarness, Simple1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    std::string s = "eliot was here";

    RecordId loc1;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), s.c_str(), s.size() + 1, Timestamp());
            ASSERT_OK(res.getStatus());
            loc1 = res.getValue();
            uow.commit();
        }

        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(s, rs->dataFor(opCtx.get(), loc1).data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(s, rs->dataFor(opCtx.get(), loc1).data());
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));

        RecordData rd;
        ASSERT(!rs->findRecord(opCtx.get(), RecordId(111, 17), &rd));
        ASSERT(rd.data() == nullptr);

        ASSERT(rs->findRecord(opCtx.get(), loc1, &rd));
        ASSERT_EQUALS(s, rd.data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), s.c_str(), s.size() + 1, Timestamp());
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, rs->numRecords(opCtx.get()));
    }
}

TEST(RecordStoreTestHarness, Delete1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    std::string s = "eliot was here";

    RecordId loc;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), s.c_str(), s.size() + 1, Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }

        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(s, rs->dataFor(opCtx.get(), loc).data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        {
            WriteUnitOfWork uow(opCtx.get());
            rs->deleteRecord(opCtx.get(), loc);
            uow.commit();
        }

        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }
}

TEST(RecordStoreTestHarness, Delete2) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    std::string s = "eliot was here";

    RecordId loc;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), s.c_str(), s.size() + 1, Timestamp());
            ASSERT_OK(res.getStatus());
            res = rs->insertRecord(opCtx.get(), s.c_str(), s.size() + 1, Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(s, rs->dataFor(opCtx.get(), loc).data());
        ASSERT_EQUALS(2, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            rs->deleteRecord(opCtx.get(), loc);
            uow.commit();
        }
    }
}

TEST(RecordStoreTestHarness, Update1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    std::string s1 = "eliot was here";
    std::string s2 = "eliot was here again";

    RecordId loc;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), s1.c_str(), s1.size() + 1, Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(s1, rs->dataFor(opCtx.get(), loc).data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            Status status = rs->updateRecord(opCtx.get(), loc, s2.c_str(), s2.size() + 1);
            ASSERT_OK(status);

            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
        ASSERT_EQUALS(s2, rs->dataFor(opCtx.get(), loc).data());
    }
}

TEST(RecordStoreTestHarness, UpdateInPlace1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    std::string s1 = "aaa111bbb";
    std::string s2 = "aaa222bbb";

    RecordId loc;
    const RecordData s1Rec(s1.c_str(), s1.size() + 1);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), s1Rec.data(), s1Rec.size(), Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(s1, rs->dataFor(opCtx.get(), loc).data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            const char* damageSource = "222";
            mutablebson::DamageVector dv;
            dv.push_back(mutablebson::DamageEvent());
            dv[0].sourceOffset = 0;
            dv[0].sourceSize = 3;
            dv[0].targetOffset = 3;
            dv[0].targetSize = 3;

            auto newRecStatus = rs->updateWithDamages(opCtx.get(), loc, s1Rec, damageSource, dv);
            ASSERT_OK(newRecStatus.getStatus());
            ASSERT_EQUALS(s2, newRecStatus.getValue().data());
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(s2, rs->dataFor(opCtx.get(), loc).data());
    }
}

TEST(RecordStoreTestHarness, Truncate1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    std::string s = "eliot was here";

    RecordId loc;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), s.c_str(), s.size() + 1, Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }


    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(s, rs->dataFor(opCtx.get(), loc).data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            rs->truncate(opCtx.get()).transitional_ignore();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }
}

TEST(RecordStoreTestHarness, Cursor1) {
    const int N = 10;

    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            for (int i = 0; i < N; i++) {
                std::string s = str::stream() << "eliot" << i;
                ASSERT_OK(rs->insertRecord(opCtx.get(), s.c_str(), s.size() + 1, Timestamp())
                              .getStatus());
            }
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(N, rs->numRecords(opCtx.get()));
    }

    {
        int x = 0;
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        auto cursor = rs->getCursor(opCtx.get());
        while (auto record = cursor->next()) {
            std::string s = str::stream() << "eliot" << x++;
            ASSERT_EQUALS(s, record->data.data());
        }
        ASSERT_EQUALS(N, x);
        ASSERT(!cursor->next());
    }

    {
        int x = N;
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_IS);
        auto cursor = rs->getCursor(opCtx.get(), false);
        while (auto record = cursor->next()) {
            std::string s = str::stream() << "eliot" << --x;
            ASSERT_EQUALS(s, record->data.data());
        }
        ASSERT_EQUALS(0, x);
        ASSERT(!cursor->next());
    }
}

TEST(RecordStoreTestHarness, CursorRestoreForward) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.a";

    auto rs = harnessHelper->newRecordStore(ns, {});
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

    Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);

    {
        WriteUnitOfWork uow(opCtx.get());
        std::string s = "test";
        for (int i = 1; i <= 3; i++) {
            ASSERT_OK(
                rs->insertRecord(opCtx.get(), RecordId(i), s.c_str(), s.size() + 1, Timestamp())
                    .getStatus());
        }
        uow.commit();
    }

    auto cursor = rs->getCursor(opCtx.get());
    auto r1 = cursor->next();
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);

    cursor->save();
    shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
    cursor->restore();

    auto r2 = cursor->next();
    ASSERT(r2);
    ASSERT_EQ(RecordId(2), r2->id);

    cursor->save();
    shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
    cursor->restore();

    auto r3 = cursor->next();
    ASSERT(r3);
    ASSERT_EQ(RecordId(3), r3->id);

    auto end = cursor->next();
    ASSERT_EQ(boost::none, end);
}

TEST(RecordStoreTestHarness, CursorRestoreReverse) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.a";

    auto rs = harnessHelper->newRecordStore(ns, {});
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);

    {
        WriteUnitOfWork uow(opCtx.get());
        std::string s = "test";
        for (int i = 1; i <= 3; i++) {
            ASSERT_OK(
                rs->insertRecord(opCtx.get(), RecordId(i), s.c_str(), s.size() + 1, Timestamp())
                    .getStatus());
        }
        uow.commit();
    }

    auto cursor = rs->getCursor(opCtx.get(), false);
    auto r1 = cursor->next();
    ASSERT(r1);
    ASSERT_EQ(RecordId(3), r1->id);

    cursor->save();
    shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
    cursor->restore();

    auto r2 = cursor->next();
    ASSERT(r2);
    ASSERT_EQ(RecordId(2), r2->id);

    cursor->save();
    shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();
    cursor->restore();

    auto r3 = cursor->next();
    ASSERT(r3);
    ASSERT_EQ(RecordId(1), r3->id);

    auto end = cursor->next();
    ASSERT_EQ(boost::none, end);
}

TEST(RecordStoreTestHarness, CursorRestoreDeletedDoc) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.a";

    auto rs = harnessHelper->newRecordStore(ns, {});
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

    Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);

    {
        WriteUnitOfWork uow(opCtx.get());
        std::string s = "test";
        for (int i = 1; i <= 3; i++) {
            ASSERT_OK(
                rs->insertRecord(opCtx.get(), RecordId(i), s.c_str(), s.size() + 1, Timestamp())
                    .getStatus());
        }
        uow.commit();
    }

    auto cursor = rs->getCursor(opCtx.get());
    auto r1 = cursor->next();
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);

    cursor->save();
    shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();

    {
        WriteUnitOfWork uow(opCtx.get());
        rs->deleteRecord(opCtx.get(), RecordId(1));
        uow.commit();
    }
    cursor->restore();

    auto r2 = cursor->next();
    ASSERT(r2);
    ASSERT_EQ(RecordId(2), r2->id);

    cursor->save();
    shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();

    {
        WriteUnitOfWork uow(opCtx.get());
        rs->deleteRecord(opCtx.get(), RecordId(2));
        uow.commit();
    }
    cursor->restore();

    auto r3 = cursor->next();
    ASSERT(r3);
    ASSERT_EQ(RecordId(3), r3->id);

    cursor->save();
    shard_role_details::getRecoveryUnit(opCtx.get())->abandonSnapshot();

    {
        WriteUnitOfWork uow(opCtx.get());
        rs->deleteRecord(opCtx.get(), RecordId(3));
        uow.commit();
    }
    cursor->restore();

    auto end = cursor->next();
    ASSERT_EQ(boost::none, end);
}

TEST(RecordStoreTestHarness, CursorSaveRestoreSeek) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.a";

    auto rs = harnessHelper->newRecordStore(ns, {});
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);

    {
        WriteUnitOfWork uow(opCtx.get());
        std::string s = "test";
        for (int i = 1; i <= 2; i++) {
            ASSERT_OK(
                rs->insertRecord(opCtx.get(), RecordId(i), s.c_str(), s.size() + 1, Timestamp())
                    .getStatus());
        }
        uow.commit();
    }

    auto cursor = rs->getCursor(opCtx.get());
    auto r1 = cursor->next();
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);

    auto r2 = cursor->next();
    ASSERT(r2);
    ASSERT_EQ(RecordId(2), r2->id);

    cursor->save();
    cursor->restore();

    r1 = cursor->seekExact(RecordId(1));
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);
}

TEST(RecordStoreTestHarness, CursorSaveUnpositionedRestoreSeek) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.a";

    auto rs = harnessHelper->newRecordStore(ns, {});
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

    Lock::GlobalLock globalLock(opCtx.get(), MODE_IX);

    {
        WriteUnitOfWork uow(opCtx.get());
        std::string s = "test";
        for (int i = 1; i <= 2; i++) {
            ASSERT_OK(
                rs->insertRecord(opCtx.get(), RecordId(i), s.c_str(), s.size() + 1, Timestamp())
                    .getStatus());
        }
        uow.commit();
    }

    auto cursor = rs->getCursor(opCtx.get());
    auto r1 = cursor->next();
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);

    auto r2 = cursor->next();
    ASSERT(r2);
    ASSERT_EQ(RecordId(2), r2->id);

    cursor->saveUnpositioned();
    cursor->restore();

    r1 = cursor->seekExact(RecordId(1));
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);
}

TEST(RecordStoreTestHarness, ClusteredRecordStore) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.system.buckets.a";
    CollectionOptions options;
    options.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
    std::unique_ptr<RecordStore> rs = harnessHelper->newRecordStore(ns, options, KeyFormat::String);
    invariant(rs->keyFormat() == KeyFormat::String);

    auto opCtx = harnessHelper->newOperationContext();
    Lock::GlobalLock globalLock(opCtx.get(), MODE_X);

    const int numRecords = 100;
    std::vector<Record> records;
    std::vector<Timestamp> timestamps(numRecords, Timestamp());

    for (int i = 0; i < numRecords; i++) {
        BSONObj doc = BSON("_id" << OID::gen() << "i" << i);
        RecordData recordData = RecordData(doc.objdata(), doc.objsize());
        recordData.makeOwned();

        RecordId id = uassertStatusOK(
            record_id_helpers::keyForDoc(doc, options.clusteredIndex->getIndexSpec(), nullptr));
        records.push_back({id, recordData});
    }

    {
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->insertRecords(opCtx.get(), &records, timestamps));
        wuow.commit();
    }

    {
        int currRecord = 0;
        auto cursor = rs->getCursor(opCtx.get(), /*forward=*/true);
        while (auto record = cursor->next()) {
            ASSERT_EQ(record->id, records.at(currRecord).id);
            ASSERT_EQ(0, strcmp(records.at(currRecord).data.data(), record->data.data()));
            currRecord++;
        }

        ASSERT_EQ(numRecords, currRecord);
    }

    if (auto cursor = rs->getRandomCursor(opCtx.get())) {
        auto record = cursor->next();
        ASSERT(record);

        auto it =
            std::find_if(records.begin(), records.end(), [&](const Record savedRecord) -> bool {
                if (savedRecord.id == record->id) {
                    return true;
                }
                return false;
            });

        ASSERT(it != records.end());
        ASSERT_EQ(0, strcmp(it->data.data(), record->data.data()));
    }

    {
        for (int i = 0; i < numRecords; i += 10) {
            RecordData rd;
            ASSERT_TRUE(rs->findRecord(opCtx.get(), records.at(i).id, &rd));
            ASSERT_EQ(0, strcmp(records.at(i).data.data(), rd.data()));
        }


        RecordId minId = record_id_helpers::keyForOID(OID());
        ASSERT_FALSE(rs->findRecord(opCtx.get(), minId, nullptr));

        RecordId maxId = record_id_helpers::keyForOID(OID::max());
        ASSERT_FALSE(rs->findRecord(opCtx.get(), maxId, nullptr));
    }

    {
        BSONObj doc = BSON("i"
                           << "updated");

        WriteUnitOfWork wuow(opCtx.get());
        for (int i = 0; i < numRecords; i += 10) {
            ASSERT_OK(
                rs->updateRecord(opCtx.get(), records.at(i).id, doc.objdata(), doc.objsize()));
        }
        wuow.commit();

        for (int i = 0; i < numRecords; i += 10) {
            RecordData rd;
            ASSERT_TRUE(rs->findRecord(opCtx.get(), records.at(i).id, &rd));
            ASSERT_EQ(0, strcmp(doc.objdata(), rd.data()));
        }
    }

    {
        WriteUnitOfWork wuow(opCtx.get());
        for (int i = 0; i < numRecords; i += 10) {
            rs->deleteRecord(opCtx.get(), records.at(i).id);
        }
        wuow.commit();

        ASSERT_EQ(numRecords - 10, rs->numRecords(opCtx.get()));
    }
}

// Verify that the internal API is able to create a capped clustered record store
// with change collection-like format. This test complements the clustered_capped_collection.js
// which verifies that we prevent a user from creating a capped clustered collections when
// enableTestCommands is disabled.
TEST(RecordStoreTestHarness, ClusteredCappedRecordStoreCreation) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "config.changes.c";
    CollectionOptions options;
    options.clusteredIndex = clustered_util::makeDefaultClusteredIdIndex();
    options.expireAfterSeconds = 1;
    options.capped = true;
    std::unique_ptr<RecordStore> rs = harnessHelper->newRecordStore(ns, options, KeyFormat::String);
    invariant(rs->keyFormat() == KeyFormat::String);
}

TEST(RecordStoreTestHarness, ClusteredCappedRecordStoreSeekNear) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.system.buckets.a";
    CollectionOptions options;
    options.capped = true;
    options.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
    std::unique_ptr<RecordStore> rs = harnessHelper->newRecordStore(ns, options, KeyFormat::String);
    invariant(rs->keyFormat() == KeyFormat::String);

    auto opCtx = harnessHelper->newOperationContext();
    Lock::GlobalLock globalLock(opCtx.get(), MODE_X);

    const int numRecords = 100;
    std::vector<Record> records;
    std::vector<Timestamp> timestamps;
    for (int i = 0; i < numRecords; i++) {
        timestamps.push_back(Timestamp(i, 1));
    }

    // Insert RecordIds where the timestamp part of the OID correlates directly with the seconds in
    // the Timestamp.
    for (int i = 0; i < numRecords; i++) {
        BSONObj doc = BSON("i" << i);
        RecordData recordData = RecordData(doc.objdata(), doc.objsize());
        recordData.makeOwned();

        auto oid = OID::gen();
        oid.setTimestamp(timestamps[i].getSecs());

        auto id = record_id_helpers::keyForOID(oid);
        auto record = Record{id, recordData};
        std::vector<Record> recVec = {record};

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_OK(rs->insertRecords(opCtx.get(), &recVec, {timestamps[i]}));
        wuow.commit();

        records.push_back(record);
    }

    for (int i = 0; i < numRecords; i++) {
        // Generate an OID RecordId with a timestamp part and high bits elsewhere such that it
        // always compares greater than or equal to the OIDs we inserted.


        auto oid = OID::max();
        oid.setTimestamp(i);

        auto rid = record_id_helpers::keyForOID(oid);
        auto cur = rs->getCursor(opCtx.get());
        auto rec = cur->seekNear(rid);
        ASSERT(rec);
        ASSERT_EQ(records[i].id, rec->id);
    }

    for (int i = 0; i < numRecords; i++) {
        // Generate an OID RecordId with only a timestamp part and zeroes elsewhere such that it
        // always compares less than or equal to the OIDs we inserted.

        auto oid = OID();
        oid.setTimestamp(i);

        auto rid = record_id_helpers::keyForOID(oid);
        auto cur = rs->getCursor(opCtx.get(), false /* forward */);
        auto rec = cur->seekNear(rid);
        ASSERT(rec);
        ASSERT_EQ(records[i].id, rec->id);
    }
}

TEST(RecordStoreTestHarness, ClusteredRecordMismatchedKeyFormat) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.system.buckets.a";
    CollectionOptions options;
    options.clusteredIndex = clustered_util::makeCanonicalClusteredInfoForLegacyFormat();
    // Cannot create a clustered record store without KeyFormat::String.
    bool failAsExpected = false;
    try {
        auto rs = harnessHelper->newRecordStore(ns, options);
    } catch (DBException& e) {
        // 6144101: WiredTiger-specific error code
        // 6144102: Ephemeral For Test-specific error code
        ASSERT_GTE(e.toStatus().code(), 6144101);
        ASSERT_LTE(e.toStatus().code(), 6144102);
        failAsExpected = true;
    }

    ASSERT_EQ(failAsExpected, true);
}

}  // namespace
}  // namespace mongo
