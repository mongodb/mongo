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

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <memory>

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

TEST(WiredTigerStandardIndexText, CursorInActiveTxnAfterNext) {
    auto harnessHelper = newSortedDataInterfaceHarnessHelper();
    bool unique = false;
    bool partial = false;
    auto sdi = harnessHelper->newSortedDataInterface(unique, partial);

    // Populate data.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        WriteUnitOfWork uow(opCtx.get());
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
        auto res = sdi->insert(opCtx.get(), ks, true);
        ASSERT_OK(res);

        ks = makeKeyString(sdi.get(), BSON("" << 2), RecordId(2));
        res = sdi->insert(opCtx.get(), ks, true);
        ASSERT_OK(res);

        uow.commit();
    }

    // Cursors should always ensure they are in an active transaction when next() is called.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        auto ru = WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtx.get()));

        auto cursor = sdi->newCursor(opCtx.get());
        auto res = cursor->seek(
            makeKeyStringForSeek(sdi.get(), BSONObj(), true, true).finishAndGetBuffer());
        ASSERT(res);

        ASSERT_TRUE(ru->isActive());

        // Committing a WriteUnitOfWork will end the current transaction.
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_TRUE(ru->isActive());
        wuow.commit();
        ASSERT_FALSE(ru->isActive());

        // If a cursor is used after a WUOW commits, it should implicitly start a new transaction.
        ASSERT(cursor->next());
        ASSERT_TRUE(ru->isActive());
    }
}

TEST(WiredTigerStandardIndexText, CursorInActiveTxnAfterSeek) {
    auto harnessHelper = newSortedDataInterfaceHarnessHelper();
    bool unique = false;
    bool partial = false;
    auto sdi = harnessHelper->newSortedDataInterface(unique, partial);

    // Populate data.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        WriteUnitOfWork uow(opCtx.get());
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
        auto res = sdi->insert(opCtx.get(), ks, true);
        ASSERT_OK(res);

        ks = makeKeyString(sdi.get(), BSON("" << 2), RecordId(2));
        res = sdi->insert(opCtx.get(), ks, true);
        ASSERT_OK(res);

        uow.commit();
    }

    // Cursors should always ensure they are in an active transaction when seek() is called.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        auto ru = WiredTigerRecoveryUnit::get(shard_role_details::getRecoveryUnit(opCtx.get()));

        auto cursor = sdi->newCursor(opCtx.get());

        bool forward = true;
        bool inclusive = true;
        auto seekKs = makeKeyStringForSeek(sdi.get(), BSON("" << 1), forward, inclusive);
        ASSERT(cursor->seek(seekKs.finishAndGetBuffer()));
        ASSERT_TRUE(ru->isActive());

        // Committing a WriteUnitOfWork will end the current transaction.
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_TRUE(ru->isActive());
        wuow.commit();
        ASSERT_FALSE(ru->isActive());

        // If a cursor is used after a WUOW commits, it should implicitly start a new
        // transaction.
        ASSERT(cursor->seek(seekKs.finishAndGetBuffer()));
        ASSERT_TRUE(ru->isActive());
    }
}

TEST(WiredTigerUniqueIndexTest, OldFormatKeys) {
    FailPointEnableBlock createOldFormatIndex("WTIndexCreateUniqueIndexesInOldFormat");

    auto harnessHelper = newSortedDataInterfaceHarnessHelper();
    const bool unique = true;
    const bool partial = false;
    auto sdi = harnessHelper->newSortedDataInterface(unique, partial);

    const bool dupsAllowed = false;

    // Populate index with all old format keys.
    {
        FailPointEnableBlock insertOldFormatKeys("WTIndexInsertUniqueKeysInOldFormat");
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        WriteUnitOfWork uow(opCtx.get());
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
        auto res = sdi->insert(opCtx.get(), ks, dupsAllowed);
        ASSERT_OK(res);

        ks = makeKeyString(sdi.get(), BSON("" << 2), RecordId(2));
        res = sdi->insert(opCtx.get(), ks, dupsAllowed);
        ASSERT_OK(res);

        ks = makeKeyString(sdi.get(), BSON("" << 3), RecordId(3));
        res = sdi->insert(opCtx.get(), ks, dupsAllowed);
        ASSERT_OK(res);

        uow.commit();
    }

    // Ensure cursors return the correct data
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        auto cursor = sdi->newCursor(opCtx.get());

        bool forward = true;
        bool inclusive = true;
        auto seekKs = makeKeyStringForSeek(sdi.get(), BSON("" << 1), forward, inclusive);
        auto record = cursor->seek(seekKs.finishAndGetBuffer());
        ASSERT(record);
        ASSERT_EQ(RecordId(1), record->loc);

        record = cursor->next();
        ASSERT(record);
        ASSERT_EQ(RecordId(2), record->loc);

        record = cursor->next();
        ASSERT(record);
        ASSERT_EQ(RecordId(3), record->loc);
    }

    // Ensure we can't insert duplicate keys in the new format
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        WriteUnitOfWork uow(opCtx.get());
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
        auto res = sdi->insert(opCtx.get(), ks, dupsAllowed);
        ASSERT_EQ(ErrorCodes::DuplicateKey, res.code());

        ks = makeKeyString(sdi.get(), BSON("" << 2), RecordId(2));
        res = sdi->insert(opCtx.get(), ks, dupsAllowed);
        ASSERT_EQ(ErrorCodes::DuplicateKey, res.code());

        ks = makeKeyString(sdi.get(), BSON("" << 3), RecordId(3));
        res = sdi->insert(opCtx.get(), ks, dupsAllowed);
        ASSERT_EQ(ErrorCodes::DuplicateKey, res.code());
    }

    // Ensure that it is not possible to remove a key with a mismatched RecordId.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        WriteUnitOfWork uow(opCtx.get());
        // The key "1" exists, but with RecordId 1, so this should not remove anything.
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(2));
        sdi->unindex(opCtx.get(), ks, dupsAllowed);
        uow.commit();

        auto cur = sdi->newCursor(opCtx.get());
        auto seekKs = makeKeyStringForSeek(sdi.get(), BSON("" << 1), true, true);
        auto result = cur->seek(seekKs.finishAndGetBuffer());
        ASSERT(result);
        ASSERT_EQ(result->loc, RecordId(1));
    }

    // Ensure we can remove an old format key and replace it with a new one.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());

        WriteUnitOfWork uow(opCtx.get());
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
        sdi->unindex(opCtx.get(), ks, dupsAllowed);

        auto res = sdi->insert(opCtx.get(), ks, dupsAllowed);
        ASSERT_OK(res);
        uow.commit();
    }
}
}  // namespace
}  // namespace mongo
