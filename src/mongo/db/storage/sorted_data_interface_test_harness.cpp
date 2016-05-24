// sorted_data_interface_test_harness.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include <algorithm>
#include <memory>

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
std::unique_ptr<SortedDataInterface> HarnessHelper::newSortedDataInterface(
    bool unique, std::initializer_list<IndexKeyEntry> toInsert) {
    invariant(std::is_sorted(
        toInsert.begin(), toInsert.end(), IndexEntryComparison(Ordering::make(BSONObj()))));

    auto index = newSortedDataInterface(unique);
    insertToIndex(this, index, toInsert);
    return index;
}

void insertToIndex(unowned_ptr<OperationContext> txn,
                   unowned_ptr<SortedDataInterface> index,
                   std::initializer_list<IndexKeyEntry> toInsert) {
    WriteUnitOfWork wuow(txn);
    for (auto&& entry : toInsert) {
        ASSERT_OK(index->insert(txn, entry.key, entry.loc, true));
    }
    wuow.commit();
}

void removeFromIndex(unowned_ptr<OperationContext> txn,
                     unowned_ptr<SortedDataInterface> index,
                     std::initializer_list<IndexKeyEntry> toRemove) {
    WriteUnitOfWork wuow(txn);
    for (auto&& entry : toRemove) {
        index->unindex(txn, entry.key, entry.loc, true);
    }
    wuow.commit();
}

TEST(SortedDataInterface, InsertWithDups1) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(5, 2), true);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(6, 2), true);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));

        long long x = 0;
        sorted->fullValidate(opCtx.get(), &x, NULL);
        ASSERT_EQUALS(2, x);
    }
}

TEST(SortedDataInterface, InsertWithDups2) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(5, 18), true);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(5, 20), true);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));
    }
}

TEST(SortedDataInterface, InsertWithDups3AndRollback) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(5, 18), true);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(5, 20), true);
            // no commit
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}

TEST(SortedDataInterface, InsertNoDups1) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(true));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(5, 18), false);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 2), RecordId(5, 20), false);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(2, sorted->numEntries(opCtx.get()));
    }
}

TEST(SortedDataInterface, InsertNoDups2) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(true));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(5, 2), false);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(5, 4), false);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}

TEST(SortedDataInterface, Unindex1) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(5, 18), true);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->unindex(opCtx.get(), BSON("" << 1), RecordId(5, 20), true);
            ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->unindex(opCtx.get(), BSON("" << 2), RecordId(5, 18), true);
            ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }


    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->unindex(opCtx.get(), BSON("" << 1), RecordId(5, 18), true);
            ASSERT(sorted->isEmpty(opCtx.get()));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT(sorted->isEmpty(opCtx.get()));
    }
}

TEST(SortedDataInterface, Unindex2Rollback) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 1), RecordId(5, 18), true);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->unindex(opCtx.get(), BSON("" << 1), RecordId(5, 18), true);
            ASSERT(sorted->isEmpty(opCtx.get()));
            // no commit
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(1, sorted->numEntries(opCtx.get()));
    }
}


TEST(SortedDataInterface, CursorIterate1) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    int N = 5;
    for (int i = 0; i < N; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            ASSERT_OK(sorted->insert(opCtx.get(), BSON("" << i), RecordId(5, i * 2), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        int n = 0;
        for (auto entry = cursor->seek(BSONObj(), true); entry; entry = cursor->next()) {
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << n), RecordId(5, n * 2)));
            n++;
        }
        ASSERT_EQUALS(N, n);
    }
}

TEST(SortedDataInterface, CursorIterate1WithSaveRestore) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    int N = 5;
    for (int i = 0; i < N; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << i), RecordId(5, i * 2), true);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        int n = 0;
        for (auto entry = cursor->seek(BSONObj(), true); entry; entry = cursor->next()) {
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << n), RecordId(5, n * 2)));
            n++;
            cursor->save();
            cursor->restore();
        }
        ASSERT_EQUALS(N, n);
    }
}


TEST(SortedDataInterface, CursorIterateAllDupKeysWithSaveRestore) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    int N = 5;
    for (int i = 0; i < N; i++) {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            sorted->insert(opCtx.get(), BSON("" << 5), RecordId(5, i * 2), true);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        int n = 0;
        for (auto entry = cursor->seek(BSONObj(), true); entry; entry = cursor->next()) {
            ASSERT_EQ(entry, IndexKeyEntry(BSON("" << 5), RecordId(5, n * 2)));
            n++;
            cursor->save();
            cursor->restore();
        }
        ASSERT_EQUALS(N, n);
    }
}


TEST(SortedDataInterface, Locate1) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    BSONObj key = BSON("" << 1);
    RecordId loc(5, 16);

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT(!cursor->seek(key, true));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());
            Status res = sorted->insert(opCtx.get(), key, loc, true);
            ASSERT_OK(res);
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(key, true), IndexKeyEntry(key, loc));
    }
}

TEST(SortedDataInterface, Locate2) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            ASSERT_OK(sorted->insert(opCtx.get(), BSON("" << 1), RecordId(1, 2), true));
            ASSERT_OK(sorted->insert(opCtx.get(), BSON("" << 2), RecordId(1, 4), true));
            ASSERT_OK(sorted->insert(opCtx.get(), BSON("" << 3), RecordId(1, 6), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(BSON("a" << 2), true), IndexKeyEntry(BSON("" << 2), RecordId(1, 4)));

        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 3), RecordId(1, 6)));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

TEST(SortedDataInterface, Locate2Empty) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            WriteUnitOfWork uow(opCtx.get());

            ASSERT_OK(sorted->insert(opCtx.get(), BSON("" << 1), RecordId(1, 2), true));
            ASSERT_OK(sorted->insert(opCtx.get(), BSON("" << 2), RecordId(1, 4), true));
            ASSERT_OK(sorted->insert(opCtx.get(), BSON("" << 3), RecordId(1, 6), true));
            uow.commit();
        }
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get()));
        ASSERT_EQ(cursor->seek(BSONObj(), true), IndexKeyEntry(BSON("" << 1), RecordId(1, 2)));
    }

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        const std::unique_ptr<SortedDataInterface::Cursor> cursor(
            sorted->newCursor(opCtx.get(), false));
        ASSERT_EQ(cursor->seek(BSONObj(), false), boost::none);
    }
}


TEST(SortedDataInterface, Locate3Descending) {
    const std::unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    const std::unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    auto buildEntry = [](int i) { return IndexKeyEntry(BSON("" << i), RecordId(1, i * 2)); };

    {
        const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        for (int i = 0; i < 10; i++) {
            if (i == 6)
                continue;
            WriteUnitOfWork uow(opCtx.get());
            auto entry = buildEntry(i);
            ASSERT_OK(sorted->insert(opCtx.get(), entry.key, entry.loc, true));
            uow.commit();
        }
    }

    const ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
    std::unique_ptr<SortedDataInterface::Cursor> cursor(sorted->newCursor(opCtx.get(), true));
    ASSERT_EQ(cursor->seek(BSON("" << 5), true), buildEntry(5));
    ASSERT_EQ(cursor->next(), buildEntry(7));

    cursor = sorted->newCursor(opCtx.get(), /*forward*/ false);
    ASSERT_EQ(cursor->seek(BSON("" << 5), /*inclusive*/ false), buildEntry(4));

    cursor = sorted->newCursor(opCtx.get(), /*forward*/ false);
    ASSERT_EQ(cursor->seek(BSON("" << 5), /*inclusive*/ true), buildEntry(5));
    ASSERT_EQ(cursor->next(), buildEntry(4));

    cursor = sorted->newCursor(opCtx.get(), /*forward*/ false);
    ASSERT_EQ(cursor->seek(BSON("" << 5), /*inclusive*/ false), buildEntry(4));
    ASSERT_EQ(cursor->next(), buildEntry(3));

    cursor = sorted->newCursor(opCtx.get(), /*forward*/ false);
    ASSERT_EQ(cursor->seek(BSON("" << 6), /*inclusive*/ true), buildEntry(5));
    ASSERT_EQ(cursor->next(), buildEntry(4));

    cursor = sorted->newCursor(opCtx.get(), /*forward*/ false);
    ASSERT_EQ(cursor->seek(BSON("" << 500), /*inclusive*/ true), buildEntry(9));
    ASSERT_EQ(cursor->next(), buildEntry(8));
}

TEST(SortedDataInterface, Locate4) {
    auto harnessHelper = newHarnessHelper();
    auto sorted = harnessHelper->newSortedDataInterface(false,
                                                        {
                                                            {BSON("" << 1), RecordId(1, 2)},
                                                            {BSON("" << 1), RecordId(1, 4)},
                                                            {BSON("" << 1), RecordId(1, 6)},
                                                            {BSON("" << 2), RecordId(1, 8)},
                                                        });

    {
        auto opCtx = harnessHelper->newOperationContext();
        auto cursor = sorted->newCursor(opCtx.get());
        ASSERT_EQ(cursor->seek(BSON("a" << 1), true), IndexKeyEntry(BSON("" << 1), RecordId(1, 2)));

        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 1), RecordId(1, 4)));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 1), RecordId(1, 6)));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 2), RecordId(1, 8)));
        ASSERT_EQ(cursor->next(), boost::none);
    }

    {
        auto opCtx = harnessHelper->newOperationContext();
        auto cursor = sorted->newCursor(opCtx.get(), false);
        ASSERT_EQ(cursor->seek(BSON("a" << 1), true), IndexKeyEntry(BSON("" << 1), RecordId(1, 6)));

        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 1), RecordId(1, 4)));
        ASSERT_EQ(cursor->next(), IndexKeyEntry(BSON("" << 1), RecordId(1, 2)));
        ASSERT_EQ(cursor->next(), boost::none);
    }
}

}  // namespace mongo
