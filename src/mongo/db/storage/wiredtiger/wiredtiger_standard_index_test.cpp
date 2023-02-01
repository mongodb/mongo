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

#include <memory>

#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection_mock.h"
#include "mongo/db/catalog/index_catalog_entry.h"
#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_index.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_recovery_unit.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_session_cache.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/system_clock_source.h"

namespace mongo {
namespace {

using std::string;

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
        auto ru = WiredTigerRecoveryUnit::get(opCtx.get());

        auto cursor = sdi->newCursor(opCtx.get());
        auto res = cursor->seek(makeKeyStringForSeek(sdi.get(), BSONObj(), true, true));
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
        auto ru = WiredTigerRecoveryUnit::get(opCtx.get());

        auto cursor = sdi->newCursor(opCtx.get());

        bool forward = true;
        bool inclusive = true;
        auto seekKs = makeKeyStringForSeek(sdi.get(), BSON("" << 1), forward, inclusive);
        ASSERT(cursor->seek(seekKs));
        ASSERT_TRUE(ru->isActive());

        // Committing a WriteUnitOfWork will end the current transaction.
        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_TRUE(ru->isActive());
        wuow.commit();
        ASSERT_FALSE(ru->isActive());

        // If a cursor is used after a WUOW commits, it should implicitly start a new
        // transaction.
        ASSERT(cursor->seek(seekKs));
        ASSERT_TRUE(ru->isActive());
    }
}

}  // namespace
}  // namespace mongo
