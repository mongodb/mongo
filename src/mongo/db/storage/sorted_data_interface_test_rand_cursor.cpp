/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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


#include "mongo/platform/basic.h"

#include "mongo/db/storage/sorted_data_interface_test_harness.h"

#include <memory>

#include "mongo/db/storage/sorted_data_interface.h"
#include "mongo/unittest/unittest.h"

using std::unique_ptr;
using std::set;
using std::string;
using std::stringstream;

namespace mongo {

// A random iterator should never return any entries from an empty index.
TEST(SortedDataInterface, GetRandomIteratorEmpty) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(true));

    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        auto cursor = sorted->newRandomCursor(opCtx.get());
        if (!cursor) {
            // newRandomCursor is not supported.
            return;
        }
        ASSERT(!cursor->next());
    }
}

// General test for 'randomness' of the cursor. With N entries in the index, we should see at least
// N/4 distinct entries after iterating N - 1 times.
TEST(SortedDataInterface, GetRandomIteratorNonEmpty) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(false));

    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        ASSERT_EQUALS(0, sorted->numEntries(opCtx.get()));
    }

    // Insert enough entries to generate multiple levels of a tree.
    const unsigned nToInsert = 5000;
    RecordId rids[nToInsert];
    for (unsigned i = 0; i < nToInsert; i++) {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        {
            BSONObj data = BSON("" << i);

            WriteUnitOfWork uow(opCtx.get());
            rids[i] = RecordId(42, i * 2);
            Status res = sorted->insert(opCtx.get(), data, rids[i], true);
            ASSERT_OK(res);
            uow.commit();
        }
    }

    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get()));
    }

    set<RecordId> remain(rids, rids + nToInsert);
    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        auto cursor = sorted->newRandomCursor(opCtx.get());
        if (!cursor) {
            // newRandomCursor is not supported.
            return;
        }

        // Iterate documents and mark those visited, but let at least one remain.
        for (unsigned i = 0; i < nToInsert - 1; i++) {
            // Get a new cursor once in a while, shouldn't affect things.
            if (i % (nToInsert / 8) == 0) {
                cursor = sorted->newRandomCursor(opCtx.get());
            }
            remain.erase(cursor->next()->loc);  // Can happen more than once per doc.
        }
        ASSERT(!remain.empty());
        ASSERT(cursor->next());

        // We should have at least visited a quarter of the items if we're any random at all
        // The expected fraction of visited entrys is 62.3%.
        ASSERT_LT(remain.size(), nToInsert * 3 / 4);
    }
}

// With only a single entry in the index, we should always receive that entry via a random cursor.
TEST(SortedDataInterface, GetRandomIteratorSingleton) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(true));

    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        ASSERT(sorted->isEmpty(opCtx.get()));
    }

    // Insert one entry.
    RecordId idToRetrieve(42, 0);
    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        WriteUnitOfWork uow(opCtx.get());
        Status res = sorted->insert(opCtx.get(), BSON("" << 0), idToRetrieve, false);
        ASSERT_OK(res);
        uow.commit();
    }

    // Double-check that the index has one entry in it now.
    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        ASSERT_EQ(1, sorted->numEntries(opCtx.get()));
    }

    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        auto cursor = sorted->newRandomCursor(opCtx.get());
        if (!cursor) {
            // newRandomCursor is not supported.
            return;
        }

        // The random cursor should keep returning the single existing entry.
        for (int i = 0; i < 10; i++) {
            auto entry = cursor->next();
            ASSERT_EQ(entry->loc, idToRetrieve);
        }

        // Check deattaching / reattaching.
        cursor->save();
        cursor->detachFromOperationContext();
        auto newClient = harnessHelper->serviceContext()->makeClient(
            "New client for SortedDataInterface::GetRandomIteratorSingleton");
        auto newOpCtx(harnessHelper->newOperationContext(newClient.get()));
        cursor->reattachToOperationContext(newOpCtx.get());
        cursor->restore();

        auto entry = cursor->next();
        ASSERT_EQ(entry->loc, idToRetrieve);

        // The random cursor should keep returning the single existing entry.
        for (int i = 0; i < 10; i++) {
            entry = cursor->next();
            ASSERT_EQ(entry->loc, idToRetrieve);
        }
    }
}

// With enough samples, we should eventually have returned every document in the tree at least once.
TEST(SortedDataInterface, GetRandomIteratorLargeDocs) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<SortedDataInterface> sorted(harnessHelper->newSortedDataInterface(true));

    // Seed the random number generator.
    harnessHelper->client()->getPrng() = PseudoRandom(1234);

    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        ASSERT_EQUALS(0, sorted->numEntries(opCtx.get()));
    }

    // Insert enough entries to generate multiple levels of a tree.
    const unsigned nToInsert = 200;
    RecordId rids[nToInsert];
    // Add a large string to the entries, to encourage more branching in the tree.
    int strBytes = 500;
    std::string keyStr('c', strBytes / sizeof('c'));
    for (unsigned i = 0; i < nToInsert; i++) {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        {
            BSONObj data = BSON("" << i << "" << keyStr);

            WriteUnitOfWork uow(opCtx.get());
            rids[i] = RecordId(42, i * 2);
            Status res = sorted->insert(opCtx.get(), data, rids[i], false);
            ASSERT_OK(res);
            uow.commit();
        }
    }

    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        ASSERT_EQUALS(nToInsert, sorted->numEntries(opCtx.get()));
    }

    set<RecordId> remain(rids, rids + nToInsert);
    {
        auto opCtx(harnessHelper->newOperationContext(harnessHelper->client()));
        auto cursor = sorted->newRandomCursor(opCtx.get());
        if (!cursor) {
            // newRandomCursor is not supported.
            return;
        }

        // Iterate the cursor enough times so that there should be a large probability of seeing all
        // the documents.
        for (unsigned i = 0; i < nToInsert * 15; i++) {
            // Get a new cursor once in a while, shouldn't affect things.
            if (i % (nToInsert / 8) == 0) {
                cursor = sorted->newRandomCursor(opCtx.get());
            }
            remain.erase(cursor->next()->loc);  // Can happen more than once per doc.
        }
        // By this point, we should have visited all entries.
        ASSERT_EQ(remain.size(), 0UL);
    }
}

}  // namespace mongo
