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
#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"

namespace mongo {
namespace {

// Add a key using a bulk builder.
TEST(SortedDataInterface, BuilderAddKey) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), true));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), key1, loc1)));
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}

/*
 * Add a KeyString using a bulk builder.
 */
TEST(SortedDataInterface, BuilderAddKeyString) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    key_string::Builder keyString1(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), true));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(keyString1.getValueCopy()));
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}

// Add a reserved RecordId using a bulk builder.
TEST(SortedDataInterface, BuilderAddKeyWithReservedRecordIdLong) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));
    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), true));

        RecordId reservedLoc(record_id_helpers::reservedIdFor(
            record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::Long));
        ASSERT(record_id_helpers::isReserved(reservedLoc));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), key1, reservedLoc)));
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}

// Add a compound key using a bulk builder.
TEST(SortedDataInterface, BuilderAddCompoundKey) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), true));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), compoundKey1a, loc1)));
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}

// Add the same key multiple times using a bulk builder and verify that duplicate key information is
// returned when duplicates are not allowed.
TEST(SortedDataInterface, BuilderAddSameKey) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), false));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), key1, loc1)));
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            builder->addKey(makeKeyString(sorted.get(), key1, loc2)), key1, boost::none);
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}

/**
 * Add the same KeyString multiple times using a bulk builder and verify that duplicate key
 * information is returned when duplicates are not allowed.
 */
TEST(SortedDataInterface, BuilderAddSameKeyString) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/true, /*partial=*/false));

    key_string::Builder keyStringLoc1(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    key_string::Builder keyStringLoc2(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc2);

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), false));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(keyStringLoc1.getValueCopy()));
        ASSERT_SDI_INSERT_DUPLICATE_KEY(
            builder->addKey(keyStringLoc2.getValueCopy()), key1, boost::none);
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}

// Add the same key multiple times using a bulk builder and verify that
// the returned status is OK when duplicates are allowed.
TEST(SortedDataInterface, BuilderAddSameKeyWithDupsAllowed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), true /* allow duplicates */));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), key1, loc1)));
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), key1, loc2)));
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));
    }
}

/*
 * Add the same KeyString multiple times using a bulk builder and verify that the returned status is
 * OK when duplicates are allowed.
 */
TEST(SortedDataInterface, BuilderAddSameKeyStringWithDupsAllowed) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    key_string::Builder keyStringLoc1(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    key_string::Builder keyStringLoc2(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc2);

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), true /* allow duplicates */));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(keyStringLoc1.getValueCopy()));
        ASSERT_SDI_INSERT_OK(builder->addKey(keyStringLoc2.getValueCopy()));
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));
    }
}

// Add multiple keys using a bulk builder.
TEST(SortedDataInterface, BuilderAddMultipleKeys) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), true));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), key1, loc1)));
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), key2, loc2)));
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), key3, loc3)));
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));
    }
}

/*
 * Add multiple KeyStrings using a bulk builder.
 */
TEST(SortedDataInterface, BuilderAddMultipleKeyStrings) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    key_string::Builder keyString1(
        sorted->getKeyStringVersion(), key1, sorted->getOrdering(), loc1);
    key_string::Builder keyString2(
        sorted->getKeyStringVersion(), key2, sorted->getOrdering(), loc2);
    key_string::Builder keyString3(
        sorted->getKeyStringVersion(), key3, sorted->getOrdering(), loc3);

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), true));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(keyString1.getValueCopy()));
        ASSERT_SDI_INSERT_OK(builder->addKey(keyString2.getValueCopy()));
        ASSERT_SDI_INSERT_OK(builder->addKey(keyString3.getValueCopy()));
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));
    }
}

// Add multiple compound keys using a bulk builder.
TEST(SortedDataInterface, BuilderAddMultipleCompoundKeys) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(
        harnessHelper->newSortedDataInterface(/*unique=*/false, /*partial=*/false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataBuilderInterface> builder(
            sorted->makeBulkBuilder(opCtx.get(), true));

        WriteUnitOfWork wuow(opCtx.get());
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), compoundKey1a, loc1)));
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), compoundKey1b, loc2)));
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), compoundKey1c, loc4)));
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), compoundKey2b, loc3)));
        ASSERT_SDI_INSERT_OK(builder->addKey(makeKeyString(sorted.get(), compoundKey3a, loc5)));
        wuow.commit();
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        Lock::GlobalLock globalLock(opCtx.get(), MODE_S);
        ASSERT_EQUALS(5, sorted->numEntries(opCtx.get()));
    }
}

}  // namespace
}  // namespace mongo
