// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/damage_vector.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/db/update/document_diff_applier.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/shared_buffer.h"

#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

// Insert a record and try to perform an in-place update on it.
void updateWithDamages(bool usePreexistingCursor, bool positionCursorCorrectly) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    ASSERT_EQUALS(0, rs->numRecords());

    std::string data = "00010111";
    RecordId loc;
    const RecordData rec(data.c_str(), data.size() + 1);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 rec.data(),
                                 rec.size(),
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }
    RecordId loc2;
    if (!positionCursorCorrectly) {
        const RecordData rec(data.c_str(), data.size() + 1);
        {
            ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
            auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
            {
                StorageWriteTransaction txn(ru);
                StatusWith<RecordId> res =
                    rs->insertRecord(opCtx.get(),
                                     *shard_role_details::getRecoveryUnit(opCtx.get()),
                                     rec.data(),
                                     rec.size(),
                                     Timestamp());
                ASSERT_OK(res.getStatus());
                loc2 = res.getValue();
                txn.commit();
            }
        }
    }

    ASSERT_EQUALS((1 + !positionCursorCorrectly), rs->numRecords());

    std::string modifiedData = "11101000";
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            DamageVector dv(3);
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

            StorageWriteTransaction txn(ru);

            if (!usePreexistingCursor) {
                auto newRecStatus = rs->updateWithDamages(
                    opCtx.get(), ru, loc, rec, data.c_str(), dv, nullptr /*cursor*/);
                ASSERT_OK(newRecStatus.getStatus());
                ASSERT_EQUALS(modifiedData, newRecStatus.getValue().data());
            } else if (positionCursorCorrectly) {
                auto cursor = rs->getCursor(opCtx.get(), ru);
                cursor->seekExact(loc);
                auto newRecStatus = rs->updateWithDamages(
                    opCtx.get(), ru, loc, rec, data.c_str(), dv, cursor.get());
                ASSERT_OK(newRecStatus.getStatus());
                ASSERT_EQUALS(modifiedData, newRecStatus.getValue().data());
            } else {
                auto cursor = rs->getCursor(opCtx.get(), ru);
                cursor->seekExact(loc2);
                auto newRecStatus = rs->updateWithDamages(
                    opCtx.get(), ru, loc, rec, data.c_str(), dv, cursor.get());
            }
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            RecordData record =
                rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc);
            ASSERT_EQUALS(modifiedData, record.data());
        }
    }
}

TEST(RecordStoreTest, UpdateWithDamages) {
    for (bool usePreexistingCursor : {false, true}) {
        updateWithDamages(usePreexistingCursor, true /*positionCursorCorrectly*/);
    }
}

DEATH_TEST(RecordStoreDeathTest, ErrorNotPositioned, "expected cursor to be positioned") {
    updateWithDamages(true /*usePreexistingCursor*/, false /*positionCursorCorrectly*/);
}

// Insert a record and try to perform an in-place update on it with a DamageVector
// containing overlapping DamageEvents.
void updateWithOverlappingDamageEvents(bool usePreexistingCursor) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    ASSERT_EQUALS(0, rs->numRecords());

    std::string data = "00010111";
    RecordId loc;
    const RecordData rec(data.c_str(), data.size() + 1);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 rec.data(),
                                 rec.size(),
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }

    ASSERT_EQUALS(1, rs->numRecords());

    std::string modifiedData = "10100010";
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            DamageVector dv(2);
            dv[0].sourceOffset = 3;
            dv[0].sourceSize = 5;
            dv[0].targetOffset = 0;
            dv[0].targetSize = 5;
            dv[1].sourceOffset = 0;
            dv[1].sourceSize = 5;
            dv[1].targetOffset = 3;
            dv[1].targetSize = 5;

            StorageWriteTransaction txn(ru);
            if (!usePreexistingCursor) {
                auto newRecStatus = rs->updateWithDamages(
                    opCtx.get(), ru, loc, rec, data.c_str(), dv, nullptr /*cursor*/);
                ASSERT_OK(newRecStatus.getStatus());
                ASSERT_EQUALS(modifiedData, newRecStatus.getValue().data());
            } else {
                auto cursor = rs->getCursor(opCtx.get(), ru);
                cursor->seekExact(loc);
                auto newRecStatus = rs->updateWithDamages(
                    opCtx.get(), ru, loc, rec, data.c_str(), dv, cursor.get());
                ASSERT_OK(newRecStatus.getStatus());
                ASSERT_EQUALS(modifiedData, newRecStatus.getValue().data());
            }
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            RecordData record =
                rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc);
            ASSERT_EQUALS(modifiedData, record.data());
        }
    }
}

TEST(RecordStoreTest, UpdateWithOverlappingDamageEvents) {
    for (bool usePreexistingCursor : {false, true}) {
        updateWithOverlappingDamageEvents(usePreexistingCursor);
    }
}

// Insert a record and try to perform an in-place update on it with a DamageVector
// containing overlapping DamageEvents. The changes should be applied in the order
// specified by the DamageVector, and not -- for instance -- by the targetOffset.
void updateWithOverlappingDamageEventsReversed(bool usePreexistingCursor) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    ASSERT_EQUALS(0, rs->numRecords());

    std::string data = "00010111";
    RecordId loc;
    const RecordData rec(data.c_str(), data.size() + 1);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 rec.data(),
                                 rec.size(),
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }

    ASSERT_EQUALS(1, rs->numRecords());

    std::string modifiedData = "10111010";
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            DamageVector dv(2);
            dv[0].sourceOffset = 0;
            dv[0].sourceSize = 5;
            dv[0].targetOffset = 3;
            dv[0].targetSize = 5;
            dv[1].sourceOffset = 3;
            dv[1].sourceSize = 5;
            dv[1].targetOffset = 0;
            dv[1].targetSize = 5;

            StorageWriteTransaction txn(ru);
            if (!usePreexistingCursor) {
                auto newRecStatus = rs->updateWithDamages(
                    opCtx.get(), ru, loc, rec, data.c_str(), dv, nullptr /*cursor*/);
                ASSERT_OK(newRecStatus.getStatus());
                ASSERT_EQUALS(modifiedData, newRecStatus.getValue().data());
            } else {
                auto cursor = rs->getCursor(opCtx.get(), ru);
                cursor->seekExact(loc);
                auto newRecStatus = rs->updateWithDamages(
                    opCtx.get(), ru, loc, rec, data.c_str(), dv, cursor.get());
                ASSERT_OK(newRecStatus.getStatus());
                ASSERT_EQUALS(modifiedData, newRecStatus.getValue().data());
            }
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            RecordData record =
                rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc);
            ASSERT_EQUALS(modifiedData, record.data());
        }
    }
}

TEST(RecordStoreTest, UpdateWithOverlappingDamageEventsReversed) {
    for (bool usePreexistingCursor : {false, true}) {
        updateWithOverlappingDamageEventsReversed(usePreexistingCursor);
    }
}

// Insert a record and try to call updateWithDamages() with an empty DamageVector.
void updateWithNoDamages(bool usePreexistingCursor) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    ASSERT_EQUALS(0, rs->numRecords());

    std::string data = "my record";
    RecordId loc;
    const RecordData rec(data.c_str(), data.size() + 1);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 rec.data(),
                                 rec.size(),
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }

    ASSERT_EQUALS(1, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            DamageVector dv;

            StorageWriteTransaction txn(ru);
            if (!usePreexistingCursor) {
                auto newRecStatus =
                    rs->updateWithDamages(opCtx.get(), ru, loc, rec, "", dv, nullptr /*cursor*/);
                ASSERT_OK(newRecStatus.getStatus());
                ASSERT_EQUALS(data, newRecStatus.getValue().data());
            } else {
                auto cursor = rs->getCursor(opCtx.get(), ru);
                cursor->seekExact(loc);
                auto newRecStatus =
                    rs->updateWithDamages(opCtx.get(), ru, loc, rec, "", dv, cursor.get());
                ASSERT_OK(newRecStatus.getStatus());
                ASSERT_EQUALS(data, newRecStatus.getValue().data());
            }
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            RecordData record =
                rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc);
            ASSERT_EQUALS(data, record.data());
        }
    }
}

TEST(RecordStoreTest, UpdateWithNoDamages) {
    for (bool usePreexistingCursor : {false, true}) {
        updateWithNoDamages(usePreexistingCursor);
    }
}

// Insert a record and try to perform inserts and updates on it.
void updateWithDamagesScalar(bool usePreexistingCursor) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    BSONObj obj0 = fromjson("{a: 1, b: 'largeStringValue'}");
    BSONObj obj1 = fromjson("{a: 1, b: 'largeStringValue', c: '12', d: 2}");
    BSONObj obj2 = fromjson("{b: 'largeStringValue', c: '123', d: 3, a: 1, e: 1}");

    RecordId loc;
    const RecordData obj0Rec(obj0.objdata(), obj0.objsize());
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 obj0Rec.data(),
                                 obj0Rec.size(),
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj0.binaryEqual(
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .toBson()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            // {i: {c: "12", d: 2}}
            auto diffOutput = doc_diff::computeOplogDiff(obj0, obj1, 0);
            ASSERT(diffOutput);
            auto [_, damageSource, damages] = doc_diff::computeDamages(obj0, *diffOutput, false);
            if (!usePreexistingCursor) {
                auto newRecStatus1 = rs->updateWithDamages(
                    opCtx.get(), ru, loc, obj0Rec, damageSource.get(), damages, nullptr /*cursor*/);
                ASSERT_OK(newRecStatus1.getStatus());
                ASSERT(obj1.binaryEqual(newRecStatus1.getValue().toBson()));
            } else {
                auto cursor = rs->getCursor(opCtx.get(), ru);
                cursor->seekExact(loc);
                auto newRecStatus1 = rs->updateWithDamages(
                    opCtx.get(), ru, loc, obj0Rec, damageSource.get(), damages, cursor.get());
                ASSERT_OK(newRecStatus1.getStatus());
                ASSERT(obj1.binaryEqual(newRecStatus1.getValue().toBson()));
            }
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj1.binaryEqual(
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .toBson()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            // {u: {c: "123", d: 3}, i: {a: 1, e: 1}}
            auto diffOutput = doc_diff::computeOplogDiff(obj1, obj2, 0);
            ASSERT(diffOutput);
            auto [_, damageSource, damages] = doc_diff::computeDamages(obj1, *diffOutput, false);
            if (!usePreexistingCursor) {
                auto newRecStatus2 = rs->updateWithDamages(opCtx.get(),
                                                           ru,
                                                           loc,
                                                           rs->dataFor(opCtx.get(), ru, loc),
                                                           damageSource.get(),
                                                           damages,
                                                           nullptr /*cursor*/);
                ASSERT_OK(newRecStatus2.getStatus());
                ASSERT(obj2.binaryEqual(newRecStatus2.getValue().toBson()));
            } else {
                auto cursor = rs->getCursor(opCtx.get(), ru);
                cursor->seekExact(loc);
                auto newRecStatus2 = rs->updateWithDamages(opCtx.get(),
                                                           ru,
                                                           loc,
                                                           rs->dataFor(opCtx.get(), ru, loc),
                                                           damageSource.get(),
                                                           damages,
                                                           cursor.get());
                ASSERT_OK(newRecStatus2.getStatus());
                ASSERT(obj2.binaryEqual(newRecStatus2.getValue().toBson()));
            }
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj2.binaryEqual(
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .toBson()));
    }
}

TEST(RecordStoreTest, UpdateWithDamagesScalar) {
    for (bool usePreexistingCursor : {false, true}) {
        updateWithDamagesScalar(usePreexistingCursor);
    }
}

// Insert a record with nested documents and try to perform updates on it.
void updateWithDamagesNested(bool usePreexistingCursor) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

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
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 obj0Rec.data(),
                                 obj0Rec.size(),
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj0.binaryEqual(
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .toBson()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            // {u: {c: "3"}, sb: {i: {q: 1}}, sd: {sp: {u: {x: {j: "1"}}}}}
            auto diffOutput = doc_diff::computeOplogDiff(obj0, obj1, 0);
            ASSERT(diffOutput);
            auto [_, damageSource, damages] = doc_diff::computeDamages(obj0, *diffOutput, true);
            if (!usePreexistingCursor) {
                auto newRecStatus1 = rs->updateWithDamages(
                    opCtx.get(), ru, loc, obj0Rec, damageSource.get(), damages, nullptr /*cursor*/);
                ASSERT_OK(newRecStatus1.getStatus());
                ASSERT(obj1.binaryEqual(newRecStatus1.getValue().toBson()));
            } else {
                auto cursor = rs->getCursor(opCtx.get(), ru);
                cursor->seekExact(loc);
                auto newRecStatus1 = rs->updateWithDamages(
                    opCtx.get(), ru, loc, obj0Rec, damageSource.get(), damages, cursor.get());
                ASSERT_OK(newRecStatus1.getStatus());
                ASSERT(obj1.binaryEqual(newRecStatus1.getValue().toBson()));
            }
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj1.binaryEqual(
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .toBson()));
    }
}

TEST(RecordStoreTest, UpdateWithDamagesNested) {
    for (bool usePreexistingCursor : {false, true}) {
        updateWithDamagesNested(usePreexistingCursor);
    }
}

// Insert a record with nested arrays and try to perform updates on it.
void updateWithDamagesArray(bool usePreexistingCursor) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    std::unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    if (!rs->updateWithDamagesSupported())
        return;

    BSONObj obj0 =
        fromjson("{field1: 'abcd', field2: [1, 2, 3, [1, 'longString', [2], 4, 5, 6], 5, 5, 5]}");
    BSONObj obj1 = fromjson("{field1: 'abcd', field2: [1, 2, 3, [1, 'longString', [4], 4], 5, 6]}");

    RecordId loc;
    const RecordData obj0Rec(obj0.objdata(), obj0.objsize());
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 obj0Rec.data(),
                                 obj0Rec.size(),
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            loc = res.getValue();
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj0.binaryEqual(
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .toBson()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            StorageWriteTransaction txn(ru);
            // {sfield2: {a: true, l: 6, 's3': {a: true, l: 4, 'u2': [4]}, 'u5': 6}}
            auto diffOutput = doc_diff::computeOplogDiff(obj0, obj1, 0);
            ASSERT(diffOutput);
            auto [_, damageSource, damages] = doc_diff::computeDamages(obj0, *diffOutput, true);
            if (!usePreexistingCursor) {
                auto newRecStatus1 = rs->updateWithDamages(
                    opCtx.get(), ru, loc, obj0Rec, damageSource.get(), damages, nullptr /*cursor*/);
                ASSERT_OK(newRecStatus1.getStatus());
                ASSERT(obj1.binaryEqual(newRecStatus1.getValue().toBson()));
            } else {
                auto cursor = rs->getCursor(opCtx.get(), ru);
                cursor->seekExact(loc);
                auto newRecStatus1 = rs->updateWithDamages(
                    opCtx.get(), ru, loc, obj0Rec, damageSource.get(), damages, cursor.get());
                ASSERT_OK(newRecStatus1.getStatus());
                ASSERT(obj1.binaryEqual(newRecStatus1.getValue().toBson()));
            }
            txn.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(obj1.binaryEqual(
            rs->dataFor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()), loc)
                .toBson()));
    }
}

TEST(RecordStoreTest, UpdateWithDamagesArray) {
    for (bool usePreexistingCursor : {false, true}) {
        updateWithDamagesArray(usePreexistingCursor);
    }
}

}  // namespace
}  // namespace mongo
