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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/record_store_test_harness.h"


#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
std::function<std::unique_ptr<RecordStoreHarnessHelper>()> recordStoreHarnessFactory;
}

void registerRecordStoreHarnessHelperFactory(
    std::function<std::unique_ptr<RecordStoreHarnessHelper>()> factory) {
    recordStoreHarnessFactory = std::move(factory);
}

auto newRecordStoreHarnessHelper() -> std::unique_ptr<RecordStoreHarnessHelper> {
    return recordStoreHarnessFactory();
}

namespace {

using std::string;
using std::unique_ptr;

TEST(RecordStoreTestHarness, Simple1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    string s = "eliot was here";

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

        ASSERT_EQUALS(s, rs->dataFor(opCtx.get(), loc1).data());
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
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
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    string s = "eliot was here";

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
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    string s = "eliot was here";

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
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    string s1 = "eliot was here";
    string s2 = "eliot was here again";

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
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
        ASSERT_EQUALS(s2, rs->dataFor(opCtx.get(), loc).data());
    }
}

TEST(RecordStoreTestHarness, UpdateInPlace1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    string s1 = "aaa111bbb";
    string s2 = "aaa222bbb";

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
            dv[0].targetOffset = 3;
            dv[0].size = 3;

            auto newRecStatus = rs->updateWithDamages(opCtx.get(), loc, s1Rec, damageSource, dv);
            ASSERT_OK(newRecStatus.getStatus());
            ASSERT_EQUALS(s2, newRecStatus.getValue().data());
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(s2, rs->dataFor(opCtx.get(), loc).data());
    }
}


TEST(RecordStoreTestHarness, Truncate1) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    string s = "eliot was here";

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
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            for (int i = 0; i < N; i++) {
                string s = str::stream() << "eliot" << i;
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
        auto cursor = rs->getCursor(opCtx.get());
        while (auto record = cursor->next()) {
            string s = str::stream() << "eliot" << x++;
            ASSERT_EQUALS(s, record->data.data());
        }
        ASSERT_EQUALS(N, x);
        ASSERT(!cursor->next());
    }

    {
        int x = N;
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getCursor(opCtx.get(), false);
        while (auto record = cursor->next()) {
            string s = str::stream() << "eliot" << --x;
            ASSERT_EQUALS(s, record->data.data());
        }
        ASSERT_EQUALS(0, x);
        ASSERT(!cursor->next());
    }
}

TEST(RecordStoreTestHarness, ClusteredRecordStore) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.system.buckets.a";
    CollectionOptions options;
    options.clusteredIndex = true;
    std::unique_ptr<RecordStore> rs = harnessHelper->newNonCappedRecordStore(ns, options);
    invariant(rs->keyFormat() == KeyFormat::String);

    auto opCtx = harnessHelper->newOperationContext();

    const int numRecords = 100;
    std::vector<Record> records;
    std::vector<Timestamp> timestamps(numRecords, Timestamp());

    for (int i = 0; i < numRecords; i++) {
        BSONObj doc = BSON("_id" << OID::gen() << "i" << i);
        RecordData recordData = RecordData(doc.objdata(), doc.objsize());
        recordData.makeOwned();

        RecordId id = uassertStatusOK(record_id_helpers::keyForDoc(doc));
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
        // Verify random cursors work on ObjectId's.
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
        // Verify that find works with ObjectId.
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
        // Verify that update works with ObjectId.
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
        // Verify that delete works with ObjectId.
        WriteUnitOfWork wuow(opCtx.get());
        for (int i = 0; i < numRecords; i += 10) {
            rs->deleteRecord(opCtx.get(), records.at(i).id);
        }
        wuow.commit();

        ASSERT_EQ(numRecords - 10, rs->numRecords(opCtx.get()));
    }
}

TEST(RecordStoreTestHarness, ClusteredRecordStoreSeekNear) {
    const auto harnessHelper = newRecordStoreHarnessHelper();
    const std::string ns = "test.system.buckets.a";
    CollectionOptions options;
    options.clusteredIndex = true;
    std::unique_ptr<RecordStore> rs = harnessHelper->newNonCappedRecordStore(ns, options);
    invariant(rs->keyFormat() == KeyFormat::String);

    auto opCtx = harnessHelper->newOperationContext();

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
}  // namespace
}  // namespace mongo
