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

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/record_id.h"
#include "mongo/db/rss/replicated_storage_service.h"
#include "mongo/db/rss/stub_persistence_provider.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options_gen.h"
#include "mongo/unittest/unittest.h"

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

class WiredTigerStandardIndexText : public SortedDataInterfaceTest {};

TEST_F(WiredTigerStandardIndexText, CursorInActiveTxnAfterNext) {
    bool unique = false;
    bool partial = false;
    auto sdi = harnessHelper()->newSortedDataInterface(opCtx(), unique, partial);

    // Populate data.
    {
        StorageWriteTransaction txn(recoveryUnit());
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
        ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks, true));

        ks = makeKeyString(sdi.get(), BSON("" << 2), RecordId(2));
        ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks, true));

        txn.commit();
    }

    // Cursors should always ensure they are in an active transaction when next() is called.
    {
        auto cursor = sdi->newCursor(opCtx(), recoveryUnit());
        auto res = cursor->seek(
            recoveryUnit(),
            makeKeyStringForSeek(sdi.get(), BSONObj(), true, true).finishAndGetBuffer());
        ASSERT(res);

        ASSERT_TRUE(recoveryUnit().isActive());

        // Committing a transaction will end the current transaction.
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_TRUE(recoveryUnit().isActive());
        txn.commit();
        ASSERT_FALSE(recoveryUnit().isActive());

        // If a cursor is used after a WUOW commits, it should implicitly start a new transaction.
        ASSERT(cursor->next(recoveryUnit()));
        ASSERT_TRUE(recoveryUnit().isActive());
    }
}

TEST_F(WiredTigerStandardIndexText, CursorInActiveTxnAfterSeek) {
    bool unique = false;
    bool partial = false;
    auto sdi = harnessHelper()->newSortedDataInterface(opCtx(), unique, partial);

    // Populate data.
    {
        StorageWriteTransaction txn(recoveryUnit());
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
        ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks, true));

        ks = makeKeyString(sdi.get(), BSON("" << 2), RecordId(2));
        ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks, true));

        txn.commit();
    }

    // Cursors should always ensure they are in an active transaction when seek() is called.
    {
        auto cursor = sdi->newCursor(opCtx(), recoveryUnit());

        bool forward = true;
        bool inclusive = true;
        auto seekKs = makeKeyStringForSeek(sdi.get(), BSON("" << 1), forward, inclusive);
        ASSERT(cursor->seek(recoveryUnit(), seekKs.finishAndGetBuffer()));
        ASSERT_TRUE(recoveryUnit().isActive());

        // Committing a transaction will end the current transaction.
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_TRUE(recoveryUnit().isActive());
        txn.commit();
        ASSERT_FALSE(recoveryUnit().isActive());

        // If a cursor is used after a WUOW commits, it should implicitly start a new
        // transaction.
        ASSERT(cursor->seek(recoveryUnit(), seekKs.finishAndGetBuffer()));
        ASSERT_TRUE(recoveryUnit().isActive());
    }
}

class WiredTigerUniqueIndexTest : public SortedDataInterfaceTest {};

TEST_F(WiredTigerUniqueIndexTest, OldFormatKeys) {
    FailPointEnableBlock createOldFormatIndex("WTIndexCreateUniqueIndexesInOldFormat");

    const bool unique = true;
    const bool partial = false;
    auto sdi = harnessHelper()->newSortedDataInterface(opCtx(), unique, partial);

    const bool dupsAllowed = false;

    // Populate index with all old format keys.
    {
        FailPointEnableBlock insertOldFormatKeys("WTIndexInsertUniqueKeysInOldFormat");
        StorageWriteTransaction txn(recoveryUnit());
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
        ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks, dupsAllowed));

        ks = makeKeyString(sdi.get(), BSON("" << 2), RecordId(2));
        ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks, dupsAllowed));

        ks = makeKeyString(sdi.get(), BSON("" << 3), RecordId(3));
        ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks, dupsAllowed));

        txn.commit();
    }

    // Ensure cursors return the correct data
    {
        auto cursor = sdi->newCursor(opCtx(), recoveryUnit());

        bool forward = true;
        bool inclusive = true;
        auto seekKs = makeKeyStringForSeek(sdi.get(), BSON("" << 1), forward, inclusive);
        auto record = cursor->seek(recoveryUnit(), seekKs.finishAndGetBuffer());
        ASSERT(record);
        ASSERT_EQ(RecordId(1), record->loc);

        record = cursor->next(recoveryUnit());
        ASSERT(record);
        ASSERT_EQ(RecordId(2), record->loc);

        record = cursor->next(recoveryUnit());
        ASSERT(record);
        ASSERT_EQ(RecordId(3), record->loc);
    }

    // Ensure we can't insert duplicate keys in the new format
    {
        StorageWriteTransaction txn(recoveryUnit());
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            sdi->insert(opCtx(), recoveryUnit(), ks, dupsAllowed), BSON("" << 1), boost::none);

        ks = makeKeyString(sdi.get(), BSON("" << 2), RecordId(2));
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            sdi->insert(opCtx(), recoveryUnit(), ks, dupsAllowed), BSON("" << 2), boost::none);

        ks = makeKeyString(sdi.get(), BSON("" << 3), RecordId(3));
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            sdi->insert(opCtx(), recoveryUnit(), ks, dupsAllowed), BSON("" << 3), boost::none);
    }

    // Ensure that it is not possible to remove a key with a mismatched RecordId.
    {
        StorageWriteTransaction txn(recoveryUnit());
        // The key "1" exists, but with RecordId 1, so this should not remove anything.
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(2));
        sdi->unindex(opCtx(), recoveryUnit(), ks, dupsAllowed);
        txn.commit();

        auto cur = sdi->newCursor(opCtx(), recoveryUnit());
        auto seekKs = makeKeyStringForSeek(sdi.get(), BSON("" << 1), true, true);
        auto result = cur->seek(recoveryUnit(), seekKs.finishAndGetBuffer());
        ASSERT(result);
        ASSERT_EQ(result->loc, RecordId(1));
    }

    // Ensure we can remove an old format key and replace it with a new one.
    {
        StorageWriteTransaction txn(recoveryUnit());
        auto ks = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
        sdi->unindex(opCtx(), recoveryUnit(), ks, dupsAllowed);

        auto res = sdi->insert(opCtx(), recoveryUnit(), ks, dupsAllowed);
        ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks, dupsAllowed));
        txn.commit();
    }
}

// Tests for blind writes on the _id index. Cover the (persistence provider gate x blind write
// ratio) matrix that gates whether WT index insert opens its cursor with overwrite=true. Only
// standbys (persistence provider returns true) at ratio>0 take the blind write path.

// Provider whose return for shouldUseBlindWriteWhenSafe can be flipped per test, simulating
// primary (false) vs standby (true).
class ConfigurableBlindWriteProvider : public rss::StubPersistenceProvider {
public:
    bool shouldUseBlindWriteWhenSafe(OperationContext*) const override {
        return _allowBlindWrite;
    }

    void setAllowBlindWrite(bool v) {
        _allowBlindWrite = v;
    }

private:
    bool _allowBlindWrite = false;
};

// Pins the (provider x ratio) matrix that gates blind writes on the _id index. Without these,
// no test exercises the allowOverwrite=true path: TestingProctor clamps the ratio to 0.0 in
// resmoke suites, so the only signal a regression here would produce is a perf regression on
// DSC standbys.
//
// Sets gWiredTigerBlindWriteRatio=1.0 for the duration of each test so the blind-write decision
// reduces to whether the persistence provider allows it. Tests flip the provider via provider()
// to simulate primary vs standby. Restoring the ratio in tearDown avoids spillover across unit
// tests.
class WiredTigerIdIndexBlindWriteTest : public SortedDataInterfaceTest {
public:
    void setUp() override {
        SortedDataInterfaceTest::setUp();
        _previousRatio = gWiredTigerBlindWriteRatio.swap(1.0);
        auto provider = std::make_unique<ConfigurableBlindWriteProvider>();
        _provider = provider.get();
        rss::ReplicatedStorageService::get(harnessHelper()->serviceContext())
            .setPersistenceProvider(std::move(provider));
    }

    void tearDown() override {
        gWiredTigerBlindWriteRatio.store(_previousRatio);
        SortedDataInterfaceTest::tearDown();
    }

    ConfigurableBlindWriteProvider* provider() {
        return _provider;
    }

private:
    ConfigurableBlindWriteProvider* _provider = nullptr;
    double _previousRatio = 0.0;
};

// Primary: provider disallows blind writes, so chooseBlindWriteOverwrite short-circuits to false
// regardless of the ratio. _id duplicates surface as DuplicateKey via WT_DUPLICATE_KEY.
TEST_F(WiredTigerIdIndexBlindWriteTest, PrimaryAlwaysReturnsDuplicateKey) {
    provider()->setAllowBlindWrite(false);

    auto sdi = harnessHelper()->newIdIndexSortedDataInterface(opCtx());

    StorageWriteTransaction txn(recoveryUnit());
    auto ks1 = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
    ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks1, /*dupsAllowed=*/false));

    auto ks2 = makeKeyString(sdi.get(), BSON("" << 1), RecordId(2));
    ASSERT_SDI_INSERT_DUPLICATE_KEY(
        sdi->insert(opCtx(), recoveryUnit(), ks2, /*dupsAllowed=*/false),
        BSON("" << 1),
        RecordId(1));
    txn.commit();
}

// Standby: provider allows blind writes, so the cursor opens with overwrite=true and a duplicate
// _id silently overwrites instead of surfacing WT_DUPLICATE_KEY. The primary already enforced
// uniqueness so the standby's local check is unnecessary.
TEST_F(WiredTigerIdIndexBlindWriteTest, StandbyOverwrites) {
    provider()->setAllowBlindWrite(true);

    auto sdi = harnessHelper()->newIdIndexSortedDataInterface(opCtx());

    StorageWriteTransaction txn(recoveryUnit());
    auto ks1 = makeKeyString(sdi.get(), BSON("" << 1), RecordId(1));
    ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks1, /*dupsAllowed=*/false));

    auto ks2 = makeKeyString(sdi.get(), BSON("" << 1), RecordId(2));
    ASSERT_SDI_INSERT_OK(sdi->insert(opCtx(), recoveryUnit(), ks2, /*dupsAllowed=*/false));
    txn.commit();
}

}  // namespace
}  // namespace mongo
