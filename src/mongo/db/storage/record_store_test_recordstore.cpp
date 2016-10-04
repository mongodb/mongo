// record_store_test_recordstore.cpp

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

namespace mongo {

// Verify that the name of the record store is not NULL and nonempty.
TEST(RecordStoreTestHarness, RecordStoreName) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        const char* name = rs->name();
        ASSERT(name != NULL && name[0] != '\0');
    }
}

// Verify that the namespace of the record store is nonempty.
TEST(RecordStoreTestHarness, Namespace) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());

    {
        string ns = rs->ns();
        ASSERT(ns[0] != '\0');
    }
}

// Call isCapped() on a non-capped collection and verify the result is false.
TEST(RecordStoreTestHarness, IsNotCapped) {
    unique_ptr<HarnessHelper> harnessHelper(newHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newNonCappedRecordStore());
    ASSERT(!rs->isCapped());
}

}  // namespace mongo
