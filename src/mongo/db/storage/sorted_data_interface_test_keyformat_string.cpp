/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include <memory>

#include "mongo/db/storage/key_string.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(SortedDataInterface, KeyFormatStringInsertDuplicates) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(
        /*unique=*/false, /*partial=*/false, KeyFormat::String));
    if (!sorted) {
        // Not supported by this storage engine.
        return;
    }
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    ASSERT(sorted->isEmpty(opCtx.get()));

    char buf1[12];
    memset(buf1, 0, 12);
    char buf2[12];
    memset(buf2, 1, 12);
    char buf3[12];
    memset(buf3, 0xff, 12);

    RecordId rid1(buf1, 12);
    RecordId rid2(buf2, 12);
    RecordId rid3(buf3, 12);

    {
        WriteUnitOfWork uow(opCtx.get());
        ASSERT_OK(sorted->insert(opCtx.get(),
                                 makeKeyString(sorted.get(), key1, rid1),
                                 /*dupsAllowed*/ true));
        ASSERT_OK(sorted->insert(opCtx.get(),
                                 makeKeyString(sorted.get(), key1, rid2),
                                 /*dupsAllowed*/ true));
        ASSERT_OK(sorted->insert(opCtx.get(),
                                 makeKeyString(sorted.get(), key1, rid3),
                                 /*dupsAllowed*/ true));
        uow.commit();
    }
    ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));

    auto ksSeek = makeKeyStringForSeek(sorted.get(), key1, true, true);
    {
        auto cursor = sorted->newCursor(opCtx.get());
        auto entry = cursor->seek(ksSeek);
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key1, rid1));

        entry = cursor->next();
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key1, rid2));

        entry = cursor->next();
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key1, rid3));
    }

    {
        auto cursor = sorted->newCursor(opCtx.get());
        auto entry = cursor->seekForKeyString(ksSeek);
        ASSERT(entry);
        ASSERT_EQ(entry->loc, rid1);
        auto ks1 = makeKeyString(sorted.get(), key1, rid1);
        ASSERT_EQ(entry->keyString, ks1);

        entry = cursor->nextKeyString();
        ASSERT(entry);
        ASSERT_EQ(entry->loc, rid2);
        auto ks2 = makeKeyString(sorted.get(), key1, rid2);
        ASSERT_EQ(entry->keyString, ks2);

        entry = cursor->nextKeyString();
        ASSERT(entry);
        ASSERT_EQ(entry->loc, rid3);
        auto ks3 = makeKeyString(sorted.get(), key1, rid3);
        ASSERT_EQ(entry->keyString, ks3);
    }
}

TEST(SortedDataInterface, KeyFormatStringSetEndPosition) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(
        /*unique=*/false, /*partial=*/false, KeyFormat::String));
    if (!sorted) {
        // Not supported by this storage engine.
        return;
    }
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    ASSERT(sorted->isEmpty(opCtx.get()));

    char buf1[12];
    memset(buf1, 0, 12);
    char buf2[12];
    memset(buf2, 1, 12);
    char buf3[12];
    memset(buf3, 0xff, 12);

    RecordId rid1(buf1, 12);
    RecordId rid2(buf2, 12);
    RecordId rid3(buf3, 12);

    {
        WriteUnitOfWork uow(opCtx.get());
        ASSERT_OK(sorted->insert(opCtx.get(),
                                 makeKeyString(sorted.get(), key1, rid1),
                                 /*dupsAllowed*/ true));
        ASSERT_OK(sorted->insert(opCtx.get(),
                                 makeKeyString(sorted.get(), key2, rid2),
                                 /*dupsAllowed*/ true));
        ASSERT_OK(sorted->insert(opCtx.get(),
                                 makeKeyString(sorted.get(), key3, rid3),
                                 /*dupsAllowed*/ true));
        uow.commit();
    }
    ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));

    // Seek for first key only
    {
        auto ksSeek = makeKeyStringForSeek(sorted.get(), key1, true, true);
        auto cursor = sorted->newCursor(opCtx.get());
        cursor->setEndPosition(key1, true /* inclusive */);
        auto entry = cursor->seek(ksSeek);
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key1, rid1));
        ASSERT_FALSE(cursor->next());
    }

    // Seek for second key from first
    {
        auto ksSeek = makeKeyStringForSeek(sorted.get(), key1, true, true);
        auto cursor = sorted->newCursor(opCtx.get());
        cursor->setEndPosition(key2, true /* inclusive */);
        auto entry = cursor->seek(ksSeek);
        ASSERT(entry);
        entry = cursor->next();
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key2, rid2));
        ASSERT_FALSE(cursor->next());
    }

    // Seek starting from the second, don't include the last key.
    {
        auto ksSeek = makeKeyStringForSeek(sorted.get(), key2, true, true);
        auto cursor = sorted->newCursor(opCtx.get());
        cursor->setEndPosition(key3, false /* inclusive */);
        auto entry = cursor->seek(ksSeek);
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key2, rid2));
        ASSERT_FALSE(cursor->next());
    }
}

TEST(SortedDataInterface, KeyFormatStringInsertReserved) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(
        /*unique=*/false, /*partial=*/false, KeyFormat::String));
    if (!sorted) {
        // Not supported by this storage engine.
        return;
    }
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    ASSERT(sorted->isEmpty(opCtx.get()));

    RecordId reservedLoc(
        RecordId::reservedIdFor<OID>(RecordId::Reservation::kWildcardMultikeyMetadataId));
    invariant(RecordId::isReserved<OID>(reservedLoc));
    {
        WriteUnitOfWork uow(opCtx.get());
        ASSERT_OK(sorted->insert(opCtx.get(),
                                 makeKeyString(sorted.get(), key1, reservedLoc),
                                 /*dupsAllowed*/ true));
        uow.commit();
    }
    ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));

    auto ksSeek = makeKeyStringForSeek(sorted.get(), key1, true, true);
    {
        auto cursor = sorted->newCursor(opCtx.get());
        auto entry = cursor->seek(ksSeek);
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key1, reservedLoc));
    }

    {
        auto cursor = sorted->newCursor(opCtx.get());
        auto entry = cursor->seekForKeyString(ksSeek);
        ASSERT(entry);
        ASSERT_EQ(entry->loc, reservedLoc);
        auto ks1 = makeKeyString(sorted.get(), key1, reservedLoc);
        ASSERT_EQ(entry->keyString, ks1);
    }
}

TEST(SortedDataInterface, KeyFormatStringUnindex) {
    const auto harnessHelper(newSortedDataInterfaceHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(
        /*unique=*/false, /*partial=*/false, KeyFormat::String));
    if (!sorted) {
        // Not supported by this storage engine.
        return;
    }
    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    ASSERT(sorted->isEmpty(opCtx.get()));

    char buf1[12];
    memset(buf1, 0, 12);
    char buf2[12];
    memset(buf2, 1, 12);
    char buf3[12];
    memset(buf3, 0xff, 12);

    RecordId rid1(buf1, 12);
    RecordId rid2(buf2, 12);
    RecordId rid3(buf3, 12);

    {
        WriteUnitOfWork uow(opCtx.get());
        ASSERT_OK(sorted->insert(opCtx.get(),
                                 makeKeyString(sorted.get(), key1, rid1),
                                 /*dupsAllowed*/ true));
        ASSERT_OK(sorted->insert(opCtx.get(),
                                 makeKeyString(sorted.get(), key1, rid2),
                                 /*dupsAllowed*/ true));
        ASSERT_OK(sorted->insert(opCtx.get(),
                                 makeKeyString(sorted.get(), key1, rid3),
                                 /*dupsAllowed*/ true));
        uow.commit();
    }
    ASSERT_EQUALS(3, sorted->numEntries(opCtx.get()));

    {
        WriteUnitOfWork uow(opCtx.get());
        sorted->unindex(opCtx.get(),
                        makeKeyString(sorted.get(), key1, rid1),
                        /*dupsAllowed*/ true);
        sorted->unindex(opCtx.get(),
                        makeKeyString(sorted.get(), key1, rid2),
                        /*dupsAllowed*/ true);
        sorted->unindex(opCtx.get(),
                        makeKeyString(sorted.get(), key1, rid3),
                        /*dupsAllowed*/ true);
        uow.commit();
    }
    ASSERT_EQUALS(0, sorted->numEntries(opCtx.get()));
}
}  // namespace
}  // namespace mongo
