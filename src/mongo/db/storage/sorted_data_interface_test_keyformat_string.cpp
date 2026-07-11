// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/record_id.h"
#include "mongo/db/record_id_helpers.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/key_format.h"
#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/db/storage/sorted_data_interface_test_assert.h"
#include "mongo/db/storage/sorted_data_interface_test_harness.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstring>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

TEST_F(SortedDataInterfaceTest, KeyFormatStringInsertDuplicates) {
    const auto sorted(harnessHelper()->newSortedDataInterface(opCtx(),
                                                              /*unique=*/false,
                                                              /*partial=*/false,
                                                              KeyFormat::String));
    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    char buf1[12];
    memset(buf1, 0, 12);
    char buf2[12];
    memset(buf2, 1, 12);
    char buf3[12];
    memset(buf3, 0xff, 12);

    RecordId rid1(buf1);
    RecordId rid2(buf2);
    RecordId rid3(buf3);

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, rid1),
                                            /*dupsAllowed*/ true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, rid2),
                                            /*dupsAllowed*/ true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, rid3),
                                            /*dupsAllowed*/ true));
        txn.commit();
    }
    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

    auto ksSeek = makeKeyStringForSeek(sorted.get(), key1, true, true);
    {
        auto cursor = sorted->newCursor(opCtx(), recoveryUnit());
        auto entry = cursor->seek(recoveryUnit(), ksSeek.finishAndGetBuffer());
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key1, rid1));

        entry = cursor->next(recoveryUnit());
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key1, rid2));

        entry = cursor->next(recoveryUnit());
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key1, rid3));
    }

    {
        auto cursor = sorted->newCursor(opCtx(), recoveryUnit());
        auto entry = cursor->seekForKeyString(recoveryUnit(), ksSeek.finishAndGetBuffer());
        ASSERT(entry);
        ASSERT_EQ(entry->loc, rid1);
        auto ks1 = makeKeyString(sorted.get(), key1, rid1);
        ASSERT_EQ(entry->keyString, ks1);

        entry = cursor->nextKeyString(recoveryUnit());
        ASSERT(entry);
        ASSERT_EQ(entry->loc, rid2);
        auto ks2 = makeKeyString(sorted.get(), key1, rid2);
        ASSERT_EQ(entry->keyString, ks2);

        entry = cursor->nextKeyString(recoveryUnit());
        ASSERT(entry);
        ASSERT_EQ(entry->loc, rid3);
        auto ks3 = makeKeyString(sorted.get(), key1, rid3);
        ASSERT_EQ(entry->keyString, ks3);
    }
}

TEST_F(SortedDataInterfaceTest, KeyFormatStringUniqueInsertRemoveDuplicates) {
    const auto sorted(harnessHelper()->newSortedDataInterface(opCtx(),
                                                              /*unique=*/true,
                                                              /*partial=*/false,
                                                              KeyFormat::String));
    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    std::string buf1(12, 0);
    std::string buf2(12, 1);
    std::string buf3(12, '\xff');

    RecordId rid1(buf1);
    RecordId rid2(buf2);
    RecordId rid3(buf3);

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, rid1),
                                            /*dupsAllowed*/ true));
        ASSERT_SDI_INSERT_KEY_EXISTS(sorted->insert(opCtx(),
                                                    recoveryUnit(),
                                                    makeKeyString(sorted.get(), key1, rid1),
                                                    /*dupsAllowed*/ false));
        ASSERT_SDI_INSERT_DUPLICATE_KEY(sorted->insert(opCtx(),
                                                       recoveryUnit(),
                                                       makeKeyString(sorted.get(), key1, rid2),
                                                       /*dupsAllowed*/ false,
                                                       IncludeDuplicateRecordId::kOff),
                                        key1,
                                        boost::none);
        ASSERT_SDI_INSERT_DUPLICATE_KEY(sorted->insert(opCtx(),
                                                       recoveryUnit(),
                                                       makeKeyString(sorted.get(), key1, rid2),
                                                       /*dupsAllowed*/ false,
                                                       IncludeDuplicateRecordId::kOn),
                                        key1,
                                        rid1);

        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, rid3),
                                            /*dupsAllowed*/ true));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), key1, rid1),
                        /*dupsAllowed*/ true);

        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key2, rid1),
                                            /*dupsAllowed*/ true));
        txn.commit();
    }

    ASSERT_EQUALS(2, sorted->numEntries(opCtx(), recoveryUnit()));

    auto ksSeek = makeKeyStringForSeek(sorted.get(), key1, true, true);
    {
        auto cursor = sorted->newCursor(opCtx(), recoveryUnit());
        auto entry = cursor->seek(recoveryUnit(), ksSeek.finishAndGetBuffer());
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key1, rid3));

        entry = cursor->next(recoveryUnit());
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key2, rid1));

        entry = cursor->next(recoveryUnit());
        ASSERT_FALSE(entry);
    }

    {
        auto cursor = sorted->newCursor(opCtx(), recoveryUnit());
        auto entry = cursor->seekForKeyString(recoveryUnit(), ksSeek.finishAndGetBuffer());
        ASSERT(entry);
        ASSERT_EQ(entry->loc, rid3);
        auto ks1 = makeKeyString(sorted.get(), key1, rid3);
        ASSERT_EQ(entry->keyString, ks1);

        entry = cursor->nextKeyString(recoveryUnit());
        ASSERT(entry);
        ASSERT_EQ(entry->loc, rid1);
        auto ks2 = makeKeyString(sorted.get(), key2, rid1);
        ASSERT_EQ(entry->keyString, ks2);

        entry = cursor->nextKeyString(recoveryUnit());
        ASSERT_FALSE(entry);
    }
}

TEST_F(SortedDataInterfaceTest, KeyFormatStringSetEndPosition) {
    const auto sorted(harnessHelper()->newSortedDataInterface(opCtx(),
                                                              /*unique=*/false,
                                                              /*partial=*/false,
                                                              KeyFormat::String));
    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    char buf1[12];
    memset(buf1, 0, 12);
    char buf2[12];
    memset(buf2, 1, 12);
    char buf3[12];
    memset(buf3, 0xff, 12);

    RecordId rid1(buf1);
    RecordId rid2(buf2);
    RecordId rid3(buf3);

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, rid1),
                                            /*dupsAllowed*/ true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key2, rid2),
                                            /*dupsAllowed*/ true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key3, rid3),
                                            /*dupsAllowed*/ true));
        txn.commit();
    }
    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

    // Seek for first key only
    {
        auto ksSeek = makeKeyStringForSeek(sorted.get(), key1, true, true);
        auto cursor = sorted->newCursor(opCtx(), recoveryUnit());
        cursor->setEndPosition(key1, true /* inclusive */);
        auto entry = cursor->seek(recoveryUnit(), ksSeek.finishAndGetBuffer());
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key1, rid1));
        ASSERT_FALSE(cursor->next(recoveryUnit()));
    }

    // Seek for second key from first
    {
        auto ksSeek = makeKeyStringForSeek(sorted.get(), key1, true, true);
        auto cursor = sorted->newCursor(opCtx(), recoveryUnit());
        cursor->setEndPosition(key2, true /* inclusive */);
        auto entry = cursor->seek(recoveryUnit(), ksSeek.finishAndGetBuffer());
        ASSERT(entry);
        entry = cursor->next(recoveryUnit());
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key2, rid2));
        ASSERT_FALSE(cursor->next(recoveryUnit()));
    }

    // Seek starting from the second, don't include the last key.
    {
        auto ksSeek = makeKeyStringForSeek(sorted.get(), key2, true, true);
        auto cursor = sorted->newCursor(opCtx(), recoveryUnit());
        cursor->setEndPosition(key3, false /* inclusive */);
        auto entry = cursor->seek(recoveryUnit(), ksSeek.finishAndGetBuffer());
        ASSERT(entry);
        ASSERT_EQ(*entry, IndexKeyEntry(key2, rid2));
        ASSERT_FALSE(cursor->next(recoveryUnit()));
    }
}

TEST_F(SortedDataInterfaceTest, KeyFormatStringUnindex) {
    const auto sorted(harnessHelper()->newSortedDataInterface(opCtx(),
                                                              /*unique=*/false,
                                                              /*partial=*/false,
                                                              KeyFormat::String));
    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    char buf1[12];
    memset(buf1, 0, 12);
    char buf2[12];
    memset(buf2, 1, 12);
    char buf3[12];
    memset(buf3, 0xff, 12);

    RecordId rid1(buf1);
    RecordId rid2(buf2);
    RecordId rid3(buf3);

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, rid1),
                                            /*dupsAllowed*/ true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, rid2),
                                            /*dupsAllowed*/ true));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, rid3),
                                            /*dupsAllowed*/ true));
        txn.commit();
    }
    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), key1, rid1),
                        /*dupsAllowed*/ true);
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), key1, rid2),
                        /*dupsAllowed*/ true);
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), key1, rid3),
                        /*dupsAllowed*/ true);
        txn.commit();
    }
    ASSERT_EQUALS(0, sorted->numEntries(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, KeyFormatStringUniqueUnindex) {
    const auto sorted(harnessHelper()->newSortedDataInterface(opCtx(),
                                                              /*unique=*/true,
                                                              /*partial=*/false,
                                                              KeyFormat::String));
    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    std::string buf1(12, 0);
    std::string buf2(12, 1);
    std::string buf3(12, '\xff');

    RecordId rid1(buf1);
    RecordId rid2(buf2);
    RecordId rid3(buf3);

    {
        StorageWriteTransaction txn(recoveryUnit());
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key1, rid1),
                                            /*dupsAllowed*/ false));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key2, rid2),
                                            /*dupsAllowed*/ false));
        ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                            recoveryUnit(),
                                            makeKeyString(sorted.get(), key3, rid3),
                                            /*dupsAllowed*/ false));
        txn.commit();
    }
    ASSERT_EQUALS(3, sorted->numEntries(opCtx(), recoveryUnit()));

    {
        StorageWriteTransaction txn(recoveryUnit());
        // Does not exist, does nothing.
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), key1, rid3),
                        /*dupsAllowed*/ false);

        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), key1, rid1),
                        /*dupsAllowed*/ false);
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), key2, rid2),
                        /*dupsAllowed*/ false);
        sorted->unindex(opCtx(),
                        recoveryUnit(),
                        makeKeyString(sorted.get(), key3, rid3),
                        /*dupsAllowed*/ false);

        txn.commit();
    }
    ASSERT_EQUALS(0, sorted->numEntries(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, InsertReservedRecordIdStr) {
    const auto sorted(harnessHelper()->newSortedDataInterface(opCtx(),
                                                              /*unique=*/false,
                                                              /*partial=*/false,
                                                              KeyFormat::String));
    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));
    StorageWriteTransaction txn(recoveryUnit());
    RecordId reservedLoc(record_id_helpers::reservedIdFor(
        record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::String));
    invariant(record_id_helpers::isReserved(reservedLoc));
    ASSERT_SDI_INSERT_OK(sorted->insert(opCtx(),
                                        recoveryUnit(),
                                        makeKeyString(sorted.get(), key1, reservedLoc),
                                        /*dupsAllowed*/ true));
    txn.commit();
    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

TEST_F(SortedDataInterfaceTest, BuilderAddKeyWithReservedRecordIdStr) {
    const auto sorted(harnessHelper()->newSortedDataInterface(opCtx(),
                                                              /*unique=*/false,
                                                              /*partial=*/false,
                                                              KeyFormat::String));
    ASSERT(sorted->isEmpty(opCtx(), recoveryUnit()));

    {
        const auto builder(sorted->makeBulkBuilder(opCtx(), recoveryUnit()));

        RecordId reservedLoc(record_id_helpers::reservedIdFor(
            record_id_helpers::ReservationId::kWildcardMultikeyMetadataId, KeyFormat::String));
        ASSERT(record_id_helpers::isReserved(reservedLoc));

        StorageWriteTransaction txn(recoveryUnit());
        builder->addKey(recoveryUnit(), makeKeyString(sorted.get(), key1, reservedLoc));
        txn.commit();
    }

    ASSERT_EQUALS(1, sorted->numEntries(opCtx(), recoveryUnit()));
}

}  // namespace
}  // namespace mongo
