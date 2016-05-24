// record_store_test_touch.cpp

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


#include "mongo/db/storage/record_store.h"
#include "mongo/unittest/unittest.h"

using std::unique_ptr;
using std::string;
using std::stringstream;

namespace mongo {

// Verify that calling touch() on an empty collection returns an OK status.
TEST(RecordStoreTestHarness, TouchEmpty) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(
            harnessHelper->newOperationContext(harnessHelper->client()));
        {
            BSONObjBuilder stats;
            Status status = rs->touch(opCtx.get(), &stats);
            ASSERT(status.isOK() || status.code() == ErrorCodes::CommandNotSupported);
        }
    }
}

// Insert multiple records, and verify that calling touch() on a nonempty collection
// returns an OK status.
TEST(RecordStoreTestHarness, TouchNonEmpty) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, false);
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(
            harnessHelper->newOperationContext(harnessHelper->client()));
        {
            BSONObjBuilder stats;
            // XXX does not verify the collection was loaded into cache
            // (even if supported by storage engine)
            Status status = rs->touch(opCtx.get(), &stats);
            ASSERT(status.isOK() || status.code() == ErrorCodes::CommandNotSupported);
        }
    }
}

// Verify that calling touch() on an empty collection returns an OK status,
// even when NULL is passed in for the stats output.
TEST(RecordStoreTestHarness, TouchEmptyWithNullStats) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(
            harnessHelper->newOperationContext(harnessHelper->client()));
        Status status = rs->touch(opCtx.get(), NULL /* stats output */);
        ASSERT(status.isOK() || status.code() == ErrorCodes::CommandNotSupported);
    }
}

// Insert multiple records, and verify that calling touch() on a nonempty collection
// returns an OK status, even when NULL is passed in for the stats output.
TEST(RecordStoreTestHarness, TouchNonEmptyWithNullStats) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(0, rs->numRecords(opCtx.get()));
    }

    int nToInsert = 10;
    for (int i = 0; i < nToInsert; i++) {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        {
            stringstream ss;
            ss << "record " << i;
            string data = ss.str();

            WriteUnitOfWork uow(opCtx.get());
            StatusWith<RecordId> res =
                rs->insertRecord(opCtx.get(), data.c_str(), data.size() + 1, false);
            ASSERT_OK(res.getStatus());
            uow.commit();
        }
    }

    {
        ServiceContext::UniqueOperationContext opCtx(harnessHelper->newOperationContext());
        ASSERT_EQUALS(nToInsert, rs->numRecords(opCtx.get()));
    }

    {
        ServiceContext::UniqueOperationContext opCtx(
            harnessHelper->newOperationContext(harnessHelper->client()));
        // XXX does not verify the collection was loaded into cache
        // (even if supported by storage engine)
        Status status = rs->touch(opCtx.get(), NULL /* stats output */);
        ASSERT(status.isOK() || status.code() == ErrorCodes::CommandNotSupported);
    }
}

}  // namespace mongo
