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
