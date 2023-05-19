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


#include "mongo/bson/json.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using std::string;
using std::unique_ptr;

// Insert a record and try to perform an in-place update on it.
TEST(RecordStoreTestHarness, UpdateWithDamages) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    string data = "00010111";
    RecordId loc;
    const RecordData rec(data.c_str(), data.size() + 1);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), rec.data(), rec.size(), Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
    }

    string modifiedData = "11101000";
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            mutablebson::DamageVector dv(3);
            dv[0].sourceOffset = 5;
            dv[0].sourceSize = 2;
            dv[0].targetOffset = 0;
            dv[0].targetSize = 2;
            dv[1].sourceOffset = 3;
            dv[1].sourceSize = 3;
            dv[1].targetOffset = 2;
            dv[1].targetSize = 3;
            dv[2].sourceOffset = 0;
            dv[2].sourceSize = 3;
            dv[2].targetOffset = 5;
            dv[2].targetSize = 3;

            WriteUnitOfWork uow(opCtx.get());
            auto newRecStatus = rs->updateWithDamages(opCtx.get(), loc, rec, data.c_str(), dv);
            ASSERT_OK(newRecStatus.getStatus());
            ASSERT_EQUALS(modifiedData, newRecStatus.getValue().data());
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            RecordData record = rs->dataFor(opCtx.get(), loc);
            ASSERT_EQUALS(modifiedData, record.data());
        }
    }
}

// Insert a record and try to perform an in-place update on it with a DamageVector
// containing overlapping DamageEvents.
TEST(RecordStoreTestHarness, UpdateWithOverlappingDamageEvents) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    string data = "00010111";
    RecordId loc;
    const RecordData rec(data.c_str(), data.size() + 1);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), rec.data(), rec.size(), Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
    }

    string modifiedData = "10100010";
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            mutablebson::DamageVector dv(2);
            dv[0].sourceOffset = 3;
            dv[0].sourceSize = 5;
            dv[0].targetOffset = 0;
            dv[0].targetSize = 5;
            dv[1].sourceOffset = 0;
            dv[1].sourceSize = 5;
            dv[1].targetOffset = 3;
            dv[1].targetSize = 5;

            WriteUnitOfWork uow(opCtx.get());
            auto newRecStatus = rs->updateWithDamages(opCtx.get(), loc, rec, data.c_str(), dv);
            ASSERT_OK(newRecStatus.getStatus());
            ASSERT_EQUALS(modifiedData, newRecStatus.getValue().data());
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            RecordData record = rs->dataFor(opCtx.get(), loc);
            ASSERT_EQUALS(modifiedData, record.data());
        }
    }
}

// Insert a record and try to perform an in-place update on it with a DamageVector
// containing overlapping DamageEvents. The changes should be applied in the order
// specified by the DamageVector, and not -- for instance -- by the targetOffset.
TEST(RecordStoreTestHarness, UpdateWithOverlappingDamageEventsReversed) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    string data = "00010111";
    RecordId loc;
    const RecordData rec(data.c_str(), data.size() + 1);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), rec.data(), rec.size(), Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
    }

    string modifiedData = "10111010";
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            mutablebson::DamageVector dv(2);
            dv[0].sourceOffset = 0;
            dv[0].sourceSize = 5;
            dv[0].targetOffset = 3;
            dv[0].targetSize = 5;
            dv[1].sourceOffset = 3;
            dv[1].sourceSize = 5;
            dv[1].targetOffset = 0;
            dv[1].targetSize = 5;

            WriteUnitOfWork uow(opCtx.get());
            auto newRecStatus = rs->updateWithDamages(opCtx.get(), loc, rec, data.c_str(), dv);
            ASSERT_OK(newRecStatus.getStatus());
            ASSERT_EQUALS(modifiedData, newRecStatus.getValue().data());
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            RecordData record = rs->dataFor(opCtx.get(), loc);
            ASSERT_EQUALS(modifiedData, record.data());
        }
    }
}

// Insert a record and try to call updateWithDamages() with an empty DamageVector.
TEST(RecordStoreTestHarness, UpdateWithNoDamages) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    string data = "my record";
    RecordId loc;
    const RecordData rec(data.c_str(), data.size() + 1);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), rec.data(), rec.size(), Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            mutablebson::DamageVector dv;

            WriteUnitOfWork uow(opCtx.get());
            auto newRecStatus = rs->updateWithDamages(opCtx.get(), loc, rec, "", dv);
            ASSERT_OK(newRecStatus.getStatus());
            ASSERT_EQUALS(data, newRecStatus.getValue().data());
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            RecordData record = rs->dataFor(opCtx.get(), loc);
            ASSERT_EQUALS(data, record.data());
        }
    }
}

// Insert a record and try to perform inserts and updates on it.
TEST(RecordStoreTestHarness, UpdateWithDamagesScalar) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    BSONObj obj0 = fromjson("{a: 1, b: 'largeStringValue'}");
    BSONObj obj1 = fromjson("{a: 1, b: 'largeStringValue', c: '12', d: 2}");
    BSONObj obj2 = fromjson("{b: 'largeStringValue', c: '123', d: 3, a: 1, e: 1}");

    RecordId loc;
    const RecordData obj0Rec(obj0.objdata(), obj0.objsize());
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), obj0Rec.data(), obj0Rec.size(), Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj0.binaryEqual(rs->dataFor(opCtx.get(), loc).toBson()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            // {i: {c: "12", d: 2}}
            auto diffOutput = doc_diff::computeOplogDiff(obj0, obj1, 0);
            ASSERT(diffOutput);
            auto [_, damageSource, damages] = doc_diff::computeDamages(obj0, *diffOutput, false);
            auto newRecStatus1 =
                rs->updateWithDamages(opCtx.get(), loc, obj0Rec, damageSource.get(), damages);
            ASSERT_OK(newRecStatus1.getStatus());
            ASSERT(obj1.binaryEqual(newRecStatus1.getValue().toBson()));
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj1.binaryEqual(rs->dataFor(opCtx.get(), loc).toBson()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            // {u: {c: "123", d: 3}, i: {a: 1, e: 1}}
            auto diffOutput = doc_diff::computeOplogDiff(obj1, obj2, 0);
            ASSERT(diffOutput);
            auto [_, damageSource, damages] = doc_diff::computeDamages(obj1, *diffOutput, false);
            auto newRecStatus2 = rs->updateWithDamages(
                opCtx.get(), loc, rs->dataFor(opCtx.get(), loc), damageSource.get(), damages);
            ASSERT_OK(newRecStatus2.getStatus());
            ASSERT(obj2.binaryEqual(newRecStatus2.getValue().toBson()));
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj2.binaryEqual(rs->dataFor(opCtx.get(), loc).toBson()));
    }
}

// Insert a record with nested documents and try to perform updates on it.
TEST(RecordStoreTestHarness, UpdateWithDamagesNested) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    BSONObj obj0 = fromjson(
        "{a: 0, "
        " b: {p: 1, q: 1, r: 2}, "
        " c: 3, "
        " d: {p: {x: {i: 1}, y: {q: 1}}}}");
    BSONObj obj1 = fromjson(
        "{a: 0, "
        " b: {p: 1, r: 2, q: 1}, "
        " c: '3', "
        " d: {p: {x: {j: '1'}, y: {q: 1}}}}");

    RecordId loc;
    const RecordData obj0Rec(obj0.objdata(), obj0.objsize());
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), obj0Rec.data(), obj0Rec.size(), Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj0.binaryEqual(rs->dataFor(opCtx.get(), loc).toBson()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            // {u: {c: "3"}, sb: {i: {q: 1}}, sd: {sp: {u: {x: {j: "1"}}}}}
            auto diffOutput = doc_diff::computeOplogDiff(obj0, obj1, 0);
            ASSERT(diffOutput);
            auto [_, damageSource, damages] = doc_diff::computeDamages(obj0, *diffOutput, true);
            auto newRecStatus1 =
                rs->updateWithDamages(opCtx.get(), loc, obj0Rec, damageSource.get(), damages);
            ASSERT_OK(newRecStatus1.getStatus());
            ASSERT(obj1.binaryEqual(newRecStatus1.getValue().toBson()));
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj1.binaryEqual(rs->dataFor(opCtx.get(), loc).toBson()));
    }
}

// Insert a record with nested arrays and try to perform updates on it.
TEST(RecordStoreTestHarness, UpdateWithDamagesArray) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    BSONObj obj0 =
        fromjson("{field1: 'abcd', field2: [1, 2, 3, [1, 'longString', [2], 4, 5, 6], 5, 5, 5]}");
    BSONObj obj1 = fromjson("{field1: 'abcd', field2: [1, 2, 3, [1, 'longString', [4], 4], 5, 6]}");

    RecordId loc;
    const RecordData obj0Rec(obj0.objdata(), obj0.objsize());
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), obj0Rec.data(), obj0Rec.size(), Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj0.binaryEqual(rs->dataFor(opCtx.get(), loc).toBson()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            // {sfield2: {a: true, l: 6, 's3': {a: true, l: 4, 'u2': [4]}, 'u5': 6}}
            auto diffOutput = doc_diff::computeOplogDiff(obj0, obj1, 0);
            ASSERT(diffOutput);
            auto [_, damageSource, damages] = doc_diff::computeDamages(obj0, *diffOutput, true);
            auto newRecStatus1 =
                rs->updateWithDamages(opCtx.get(), loc, obj0Rec, damageSource.get(), damages);
            ASSERT_OK(newRecStatus1.getStatus());
            ASSERT(obj1.binaryEqual(newRecStatus1.getValue().toBson()));
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj1.binaryEqual(rs->dataFor(opCtx.get(), loc).toBson()));
    }
}

}  // namespace
}  // namespace mongo
