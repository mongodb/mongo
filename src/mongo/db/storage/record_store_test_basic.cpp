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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST(RecordStoreTest, Simple1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    std::string s = "eliot was here";

    RecordId loc1;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 s.c_str(),
                                 s.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc1 = res.getValue();
            txn.commit();
        }

        ASSERT_EQUALS(s, rs->dataFor(opCtx.get(), ru, loc1).data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(
            s,
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc1)
                .data());
        ASSERT_EQUALS(1, rs->numRecords());

        RecordData rd;
        ASSERT(!rs->findRecord(opCtx.get(),
                               *shard_role_details::getRecoveryUnit(opCtx.get()),
                               RecordId(111, 17),
                               &rd));
        ASSERT(rd.data() == nullptr);

        ASSERT(rs->findRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc1, &rd));
        ASSERT_EQUALS(s, rd.data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 s.c_str(),
                                 s.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            txn.commit();
        }
    }

    ASSERT_EQUALS(2, rs->numRecords());
}

TEST(RecordStoreTest, Delete1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    std::string s = "eliot was here";

    RecordId loc;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 s.c_str(),
                                 s.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }

        ASSERT_EQUALS(s, rs->dataFor(opCtx.get(), ru, loc).data());
    }

    ASSERT_EQUALS(1, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        {
            StorageWriteTransaction txn(ru);
            rs->deleteRecord(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc);
            txn.commit();
        }

        ASSERT_EQUALS(0, rs->numRecords());
    }
}

TEST(RecordStoreTest, Delete2) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    std::string s = "eliot was here";

    RecordId loc;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 s.c_str(),
                                 s.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            res = rs->insertRecord(opCtx.get(),
                                   *shard_role_details::getRecoveryUnit(opCtx.get()),
                                   s.c_str(),
                                   s.size() + 1,
                                   Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(
            s,
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .data());
        ASSERT_EQUALS(2, rs->numRecords());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            rs->deleteRecord(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc);
            txn.commit();
        }
    }
}

TEST(RecordStoreTest, Update1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords());
    }

    std::string s1 = "eliot was here";
    std::string s2 = "eliot was here again";

    RecordId loc;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 s1.c_str(),
                                 s1.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(
            s1,
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            Status status = rs->updateRecord(opCtx.get(), ru, loc, s2.c_str(), s2.size() + 1);
            ASSERT_OK(status);

            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, rs->numRecords());
        ASSERT_EQUALS(
            s2,
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .data());
    }
}

TEST(RecordStoreTest, UpdateInPlace1) {
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
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 s1Rec.data(),
                                 s1Rec.size(),
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(
            s1,
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            const char* damageSource = "222";
            DamageVector dv;
            dv.push_back(DamageEvent());
            dv[0].sourceOffset = 0;
            dv[0].sourceSize = 3;
            dv[0].targetOffset = 3;
            dv[0].targetSize = 3;

            auto newRecStatus =
                rs->updateWithDamages(opCtx.get(),
                                      *shard_role_details::getRecoveryUnit(opCtx.get()),
                                      loc,
                                      s1Rec,
                                      damageSource,
                                      dv);
            ASSERT_OK(newRecStatus.getStatus());
            ASSERT_EQUALS(s2, newRecStatus.getValue().data());
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(
            s2,
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .data());
    }
}

TEST(RecordStoreTest, Truncate1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    std::string s = "eliot was here";

    RecordId loc;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 s.c_str(),
                                 s.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }


    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(
            s,
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .data());
    }

    ASSERT_EQUALS(1, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            rs->truncate(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()))
                .transitional_ignore();
            txn.commit();
        }
    }

    ASSERT_EQUALS(0, rs->numRecords());
}

TEST(RecordStoreTest, Cursor1) {
    const int N = 10;

    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            for (int i = 0; i < N; i++) {
                std::string s = str::stream() << "eliot" << i;
                ASSERT_OK(rs->insertRecord(opCtx.get(),
                                           *shard_role_details::getRecoveryUnit(opCtx.get()),
                                           s.c_str(),
                                           s.size() + 1,
                                           Timestamp())
                              .getStatus());
            }
            txn.commit();
        }
    }

    ASSERT_EQUALS(N, rs->numRecords());

    {
        int x = 0;
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
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
        auto cursor =
            rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), false);
        while (auto record = cursor->next()) {
            std::string s = str::stream() << "eliot" << --x;
            ASSERT_EQUALS(s, record->data.data());
        }
        ASSERT_EQUALS(0, x);
        ASSERT(!cursor->next());
    }
}

TEST(RecordStoreTest, CursorRestoreForward) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.a";

    auto rs = harnessHelper->newRecordStore(ns, {});
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());


    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    {
        StorageWriteTransaction txn(ru);
        std::string s = "test";
        for (int i = 1; i <= 3; i++) {
            ASSERT_OK(rs->insertRecord(opCtx.get(),
                                       *shard_role_details::getRecoveryUnit(opCtx.get()),
                                       RecordId(i),
                                       s.c_str(),
                                       s.size() + 1,
                                       Timestamp())
                          .getStatus());
        }
        txn.commit();
    }

    auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
    auto r1 = cursor->next();
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);

    cursor->save();
    ru.abandonSnapshot();
    cursor->restore(ru);

    auto r2 = cursor->next();
    ASSERT(r2);
    ASSERT_EQ(RecordId(2), r2->id);

    cursor->save();
    ru.abandonSnapshot();
    cursor->restore(ru);

    auto r3 = cursor->next();
    ASSERT(r3);
    ASSERT_EQ(RecordId(3), r3->id);

    auto end = cursor->next();
    ASSERT_EQ(boost::none, end);
}

TEST(RecordStoreTest, CursorRestoreReverse) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.a";

    auto rs = harnessHelper->newRecordStore(ns, {});
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    {
        StorageWriteTransaction txn(ru);
        std::string s = "test";
        for (int i = 1; i <= 3; i++) {
            ASSERT_OK(rs->insertRecord(opCtx.get(),
                                       *shard_role_details::getRecoveryUnit(opCtx.get()),
                                       RecordId(i),
                                       s.c_str(),
                                       s.size() + 1,
                                       Timestamp())
                          .getStatus());
        }
        txn.commit();
    }

    auto cursor =
        rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), false);
    auto r1 = cursor->next();
    ASSERT(r1);
    ASSERT_EQ(RecordId(3), r1->id);

    cursor->save();
    ru.abandonSnapshot();
    cursor->restore(ru);

    auto r2 = cursor->next();
    ASSERT(r2);
    ASSERT_EQ(RecordId(2), r2->id);

    cursor->save();
    ru.abandonSnapshot();
    cursor->restore(ru);

    auto r3 = cursor->next();
    ASSERT(r3);
    ASSERT_EQ(RecordId(1), r3->id);

    auto end = cursor->next();
    ASSERT_EQ(boost::none, end);
}

TEST(RecordStoreTest, CursorRestoreDeletedDoc) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.a";

    auto rs = harnessHelper->newRecordStore(ns, {});
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    {
        StorageWriteTransaction txn(ru);
        std::string s = "test";
        for (int i = 1; i <= 3; i++) {
            ASSERT_OK(rs->insertRecord(opCtx.get(),
                                       *shard_role_details::getRecoveryUnit(opCtx.get()),
                                       RecordId(i),
                                       s.c_str(),
                                       s.size() + 1,
                                       Timestamp())
                          .getStatus());
        }
        txn.commit();
    }

    auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
    auto r1 = cursor->next();
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);

    cursor->save();
    ru.abandonSnapshot();

    {
        StorageWriteTransaction txn(ru);
        rs->deleteRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), RecordId(1));
        txn.commit();
    }
    cursor->restore(ru);

    auto r2 = cursor->next();
    ASSERT(r2);
    ASSERT_EQ(RecordId(2), r2->id);

    cursor->save();
    ru.abandonSnapshot();

    {
        StorageWriteTransaction txn(ru);
        rs->deleteRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), RecordId(2));
        txn.commit();
    }
    cursor->restore(ru);

    auto r3 = cursor->next();
    ASSERT(r3);
    ASSERT_EQ(RecordId(3), r3->id);

    cursor->save();
    ru.abandonSnapshot();

    {
        StorageWriteTransaction txn(ru);
        rs->deleteRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), RecordId(3));
        txn.commit();
    }
    cursor->restore(ru);

    auto end = cursor->next();
    ASSERT_EQ(boost::none, end);
}

TEST(RecordStoreTest, CursorSaveRestoreSeek) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.a";

    auto rs = harnessHelper->newRecordStore(ns, {});
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    {
        StorageWriteTransaction txn(ru);
        std::string s = "test";
        for (int i = 1; i <= 2; i++) {
            ASSERT_OK(rs->insertRecord(opCtx.get(),
                                       *shard_role_details::getRecoveryUnit(opCtx.get()),
                                       RecordId(i),
                                       s.c_str(),
                                       s.size() + 1,
                                       Timestamp())
                          .getStatus());
        }
        txn.commit();
    }

    auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
    auto r1 = cursor->next();
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);

    auto r2 = cursor->next();
    ASSERT(r2);
    ASSERT_EQ(RecordId(2), r2->id);

    cursor->save();
    cursor->restore(ru);

    r1 = cursor->seekExact(RecordId(1));
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);
}

TEST(RecordStoreTest, CursorSaveUnpositionedRestoreSeek) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.a";

    auto rs = harnessHelper->newRecordStore(ns, {});
    ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

    {
        StorageWriteTransaction txn(ru);
        std::string s = "test";
        for (int i = 1; i <= 2; i++) {
            ASSERT_OK(rs->insertRecord(opCtx.get(),
                                       *shard_role_details::getRecoveryUnit(opCtx.get()),
                                       RecordId(i),
                                       s.c_str(),
                                       s.size() + 1,
                                       Timestamp())
                          .getStatus());
        }
        txn.commit();
    }

    auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
    auto r1 = cursor->next();
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);

    auto r2 = cursor->next();
    ASSERT(r2);
    ASSERT_EQ(RecordId(2), r2->id);

    cursor->saveUnpositioned();
    cursor->restore(ru);

    r1 = cursor->seekExact(RecordId(1));
    ASSERT(r1);
    ASSERT_EQ(RecordId(1), r1->id);
}

TEST(RecordStoreTest, ClusteredRecordStore) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "testDB.clusteredColl";
    RecordStore::Options rsOptions = harnessHelper->clusteredRecordStoreOptions();
    std::unique_ptr<RecordStore> rs = harnessHelper->newRecordStore(ns, rsOptions);
    invariant(rs->keyFormat() == KeyFormat::String);

    const int numRecords = 100;
    std::vector<Record> records;
    std::vector<Timestamp> timestamps(numRecords, Timestamp());

    for (int i = 0; i < numRecords; i++) {
        auto oid = OID::gen();
        BSONObj doc = BSON("_id" << oid << "i" << i);
        RecordData recordData = RecordData(doc.objdata(), doc.objsize());
        recordData.makeOwned();
        auto rid = record_id_helpers::keyForOID(oid);
        records.push_back({rid, recordData});
    }

    auto opCtx = harnessHelper->newOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
    {
        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->insertRecords(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), &records, timestamps));
        txn.commit();
    }

    {
        int currRecord = 0;
        auto cursor = rs->getCursor(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), /*forward=*/true);
        while (auto record = cursor->next()) {
            ASSERT_EQ(record->id, records.at(currRecord).id);
            ASSERT_EQ(0, strcmp(records.at(currRecord).data.data(), record->data.data()));
            currRecord++;
        }

        ASSERT_EQ(numRecords, currRecord);
    }

    if (auto cursor =
            rs->getRandomCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()))) {
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
            ASSERT_TRUE(rs->findRecord(opCtx.get(),
                                       *shard_role_details::getRecoveryUnit(opCtx.get()),
                                       records.at(i).id,
                                       &rd));
            ASSERT_EQ(0, strcmp(records.at(i).data.data(), rd.data()));
        }


        RecordId minId = record_id_helpers::keyForOID(OID());
        ASSERT_FALSE(rs->findRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), minId, nullptr));

        RecordId maxId = record_id_helpers::keyForOID(OID::max());
        ASSERT_FALSE(rs->findRecord(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), maxId, nullptr));
    }

    {
        BSONObj doc = BSON("i" << "updated");

        StorageWriteTransaction txn(ru);
        for (int i = 0; i < numRecords; i += 10) {
            ASSERT_OK(
                rs->updateRecord(opCtx.get(), ru, records.at(i).id, doc.objdata(), doc.objsize()));
        }
        txn.commit();

        for (int i = 0; i < numRecords; i += 10) {
            RecordData rd;
            ASSERT_TRUE(rs->findRecord(opCtx.get(),
                                       *shard_role_details::getRecoveryUnit(opCtx.get()),
                                       records.at(i).id,
                                       &rd));
            ASSERT_EQ(0, strcmp(doc.objdata(), rd.data()));
        }
    }

    {
        StorageWriteTransaction txn(ru);
        for (int i = 0; i < numRecords; i += 10) {
            rs->deleteRecord(
                opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), records.at(i).id);
        }
        txn.commit();

        ASSERT_EQ(numRecords - 10, rs->numRecords());
    }
}

// Verify that the internal API is able to create a capped clustered record store
// with change collection-like format. This test complements the clustered_capped_collection.js
// which verifies that we prevent a user from creating a capped clustered collections when
// enableTestCommands is disabled.
TEST(RecordStoreTest, ClusteredCappedRecordStoreCreation) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "config.changes.c";
    RecordStore::Options rsOptions = harnessHelper->clusteredRecordStoreOptions();
    rsOptions.isCapped = true;
    std::unique_ptr<RecordStore> rs = harnessHelper->newRecordStore(ns, rsOptions);
    invariant(rs->keyFormat() == KeyFormat::String);
}

TEST(RecordStoreTest, ClusteredCappedRecordStoreSeek) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    RecordStore::Options rsOptions = harnessHelper->clusteredRecordStoreOptions();
    rsOptions.isCapped = true;
    const std::string ns = "test.clusteredCappedColl";
    std::unique_ptr<RecordStore> rs = harnessHelper->newRecordStore(ns, rsOptions);
    invariant(rs->keyFormat() == KeyFormat::String);

    auto opCtx = harnessHelper->newOperationContext();
    auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

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

        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->insertRecords(opCtx.get(),
                                    *shard_role_details::getRecoveryUnit(opCtx.get()),
                                    &recVec,
                                    {timestamps[i]}));
        txn.commit();

        records.push_back(record);
    }

    for (int i = 0; i < numRecords - 1; i++) {
        // Generate an OID RecordId with a timestamp part and high bits elsewhere such that it
        // always compares greater than or equal to the OIDs we inserted.


        auto oid = OID::max();
        oid.setTimestamp(i);

        auto rid = record_id_helpers::keyForOID(oid);
        auto cur = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        auto rec = cur->seek(rid, SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT(rec);
        ASSERT_GT(rec->id, rid);
    }

    for (int i = 1; i < numRecords; i++) {
        // Generate an OID RecordId with only a timestamp part and zeroes elsewhere such that it
        // always compares less than or equal to the OIDs we inserted.

        auto oid = OID();
        oid.setTimestamp(i);

        auto rid = record_id_helpers::keyForOID(oid);
        auto cur = rs->getCursor(
            opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), /*forward=*/false);
        auto rec = cur->seek(rid, SeekableRecordCursor::BoundInclusion::kInclude);
        ASSERT(rec);
        ASSERT_LT(rec->id, rid);
    }
}

// Verify that a failed restore leaves the _hasRestored flag unset.
DEATH_TEST_REGEX(RecordStoreTest, FailedRestoreDoesNotSetFlag, "Invariant failure.*_hasRestored") {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    auto rs(harnessHelper->newRecordStore());
    {
        auto opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());

        char data[] = "data";
        StorageWriteTransaction txn(ru);
        ASSERT_OK(rs->insertRecord(opCtx.get(),
                                   *shard_role_details::getRecoveryUnit(opCtx.get()),
                                   data,
                                   strlen(data) + 1,
                                   Timestamp()));
        txn.commit();

        auto cursor = rs->getCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        // Positions cursor at first record for save()
        ASSERT(cursor->next());
        // Clears _hasRestored
        cursor->save();
        auto restoreFailed = [&ru, &cursor]() {
            try {
                FailPointEnableBlock failPoint("WTWriteConflictExceptionForReads");
                // Should not set _hasRestored
                cursor->restore(ru);
                return false;
            } catch (const ExceptionFor<ErrorCodes::WriteConflict>&) {
                return true;
            }
        }();
        ASSERT(restoreFailed);
        // Calling next() should fail the invariant(_hasRestored)
        cursor->next();
    }
}

}  // namespace
}  // namespace mongo
