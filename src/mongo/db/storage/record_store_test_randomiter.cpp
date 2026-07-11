// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/status_with.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/record_id.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <ostream>
#include <set>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace {

using std::set;
using std::string;
using std::stringstream;
using std::unique_ptr;

// Create a random iterator for empty record store.
TEST(RecordStoreTest, GetRandomIteratorEmpty) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor =
            rs->getRandomCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        // returns NULL if getRandomCursor is not supported
        if (!cursor) {
            return;
        }
        ASSERT(!cursor->next());
    }
}

// Insert multiple records and create a random iterator for the record store
TEST(RecordStoreTest, GetRandomIteratorNonEmpty) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQUALS(0, rs->numRecords());

    const unsigned nToInsert =
        5000;  // should be non-trivial amount, so we get multiple btree levels
    RecordId locs[nToInsert];
    for (unsigned i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            StorageWriteTransaction txn(ru);
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(),
                                 *shard_role_details::getRecoveryUnit(opCtx.get()),
                                 data.c_str(),
                                 data.size() + 1,
                                 Timestamp());
            ASSERT_OK(res.getStatus());
            locs[i] = res.getValue();
            txn.commit();
        }
    }

    ASSERT_EQUALS(nToInsert, rs->numRecords());

    set<RecordId> remain(locs, locs + nToInsert);
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor =
            rs->getRandomCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
        // returns NULL if getRandomCursor is not supported
        if (!cursor) {
            return;
        }

        // Iterate documents and mark those visited, but let at least one remain
        for (unsigned i = 0; i < nToInsert - 1; i++) {
            // Get a new cursor once in a while, shouldn't affect things
            if (i % (nToInsert / 8) == 0) {
                cursor = rs->getRandomCursor(opCtx.get(),
                                             *shard_role_details::getRecoveryUnit(opCtx.get()));
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
TEST(RecordStoreTest, GetRandomIteratorSingleton) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    ASSERT_EQ(0, rs->numRecords());

    // Insert one record.
    RecordId idToRetrieve;
    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto& ru = *shard_role_details::getRecoveryUnit(opCtx.get());
        StorageWriteTransaction txn(ru);
        StatusWith<RecordId> res =
            rs->insertRecord(opCtx.get(),
                             *shard_role_details::getRecoveryUnit(opCtx.get()),
                             "some data",
                             10,
                             Timestamp());
        ASSERT_OK(res.getStatus());
        idToRetrieve = res.getValue();
        txn.commit();
    }

    // Double-check that the record store has one record in it now.
    ASSERT_EQ(1, rs->numRecords());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        auto cursor =
            rs->getRandomCursor(opCtx.get(), *shard_role_details::getRecoveryUnit(opCtx.get()));
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
        ASSERT_TRUE(cursor->restore(*shard_role_details::getRecoveryUnit(opCtx.get())));

        auto record = cursor->next();
        ASSERT_EQUALS(record->id, idToRetrieve);

        // Iterator should either be EOF now, or keep returning the single existing document
        for (int i = 0; i < 10; i++) {
            record = cursor->next();
            ASSERT(!record || record->id == idToRetrieve);
        }
    }
}
}  // namespace
}  // namespace mongo
