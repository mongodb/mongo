// record_store_test_randomiter.cpp

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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/record_store_test_harness.h"


#include "mongo/db/record_id.h"
#include "mongo/db/storage/record_data.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

using std::unique_ptr;
using std::set;
using std::string;
using std::stringstream;

namespace mongo {

// Create a random iterator for empty record store.
TEST(RecordStoreTestHarness, GetRandomIteratorEmpty) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getRandomCursor(opCtx.get());
        // returns NULL if getRandomCursor is not supported
        if (!cursor) {
            return;
        }
        ASSERT(!cursor->next());
    }
}

// Insert multiple records and create a random iterator for the record store
TEST(RecordStoreTestHarness, GetRandomIteratorNonEmpty) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    const unsigned nToInsert =
        5000;  // should be non-trivial amount, so we get multiple btree levels
    RecordId locs[nToInsert];
    for (unsigned i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, false);
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, rs->numRecords(opCtx.get()));
    }

    set<RecordId> remain(locs, locs + nToInsert);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getRandomCursor(opCtx.get());
        // returns NULL if getRandomCursor is not supported
        if (!cursor) {
            return;
        }

        // Iterate documents and mark those visited, but let at least one remain
        for (unsigned i = 0; i < nToInsert - 1; i++) {
            // Get a new cursor once in a while, shouldn't affect things
            if (i % (nToInsert / 8) == 0) {
                cursor = rs->getRandomCursor(opCtx.get());
            }
            remain.erase(cursor->next()->id);  // can happen more than once per doc
        }
        ASSERT(!remain.empty());
        ASSERT(cursor->next());

        // We should have at least visited a quarter of the items if we're any random at all
        // The expected fraction of visited records is 62.3%.
        ASSERT_LT(remain.size(), nToInsert * 3 / 4);
    }
}

// Insert a single record. Create a random iterator pointing to that single record.
// Then check we'll retrieve the record.
TEST(RecordStoreTestHarness, GetRandomIteratorSingleton) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQ(0, rs->numRecords(opCtx.get()));
    }

    // Insert one record.
    RecordId idToRetrieve;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        WriteUnitOfWork uow(opCtx.get());
        StatusWith<RecordId> res = rs->insertRecord(opCtx.get(), "some data", 10, false);
        ASSERT_OK(res.getStatus());
        idToRetrieve = res.getValue();
        uow.commit();
    }

    // Double-check that the record store has one record in it now.
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQ(1, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor = rs->getRandomCursor(opCtx.get());
        // returns NULL if getRandomCursor is not supported
        if (!cursor) {
            return;
        }

        // We should be pointing at the only record in the store.

        // Check deattaching / reattaching
        cursor->save();
        cursor->detachFromOperationContext();
        opCtx.reset();
        opCtx = harnessHelper->newOperationContext();
        cursor->reattachToOperationContext(opCtx.get());
        ASSERT_TRUE(cursor->restore());

        auto record = cursor->next();
        ASSERT_EQUALS(record->id, idToRetrieve);

        // Iterator should either be EOF now, or keep returning the single existing document
        for (int i = 0; i < 10; i++) {
            record = cursor->next();
            ASSERT(!record || record->id == idToRetrieve);
        }
    }
}
}  // namespace mongo
