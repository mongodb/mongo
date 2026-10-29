// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>

namespace mongo {
namespace {

using std::string;
using std::unique_ptr;

// Verify that the name of the record store is not NULL and nonempty.
TEST(RecordStoreTest, RecordStoreName) {
    const auto harnessHelper(newRecordStoreHarnessHelper());
    unique_ptr<RecordStore> rs(harnessHelper->newRecordStore());

    {
        const char* name = rs->name();
        ASSERT(name != nullptr && name[0] != '\0');
    }
}

}  // namespace
}  // namespace mongo
