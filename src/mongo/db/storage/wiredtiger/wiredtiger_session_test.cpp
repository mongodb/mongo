/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_session.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store_test_harness.h"
#include "mongo/unittest/unittest.h"

#include <memory>

#include <wiredtiger.h>

namespace mongo {

TEST(WiredTigerSessionTest, CacheMixedOverwrite) {
    WiredTigerHarnessHelper helper;
    std::unique_ptr<RecoveryUnit> ru = helper.newRecoveryUnit();
    std::unique_ptr<RecordStore> rs = helper.newRecordStore();
    auto uri = std::string{static_cast<WiredTigerRecordStore*>(rs.get())->getURI()};

    // Close all cached cursors to establish a 'before' state.
    auto session = static_cast<WiredTigerRecoveryUnit*>(ru.get())->getSession();
    session->closeAllCursors(uri);
    int cachedCursorsBefore = session->cachedCursors();

    // Use a large, unused table ID for this test to ensure we don't collide with any other table
    // ids.
    int tableId = 999999999;
    WT_CURSOR* cursor;

    // Expect no cached cursors.
    {
        auto config = "";
        cursor = session->getCachedCursor(tableId, config);
        ASSERT_FALSE(cursor);

        cursor = session->getNewCursor(uri, config);
        ASSERT(cursor);
        session->releaseCursor(tableId, cursor, config);
        ASSERT_GT(session->cachedCursors(), cachedCursorsBefore);
    }

    cachedCursorsBefore = session->cachedCursors();

    // Use a different overwrite setting, expect no cached cursors.
    {
        auto config = "overwrite=false";
        cursor = session->getCachedCursor(tableId, config);
        ASSERT_FALSE(cursor);

        cursor = session->getNewCursor(uri, config);
        ASSERT(cursor);
        session->releaseCursor(tableId, cursor, config);
        ASSERT_GT(session->cachedCursors(), cachedCursorsBefore);
    }

    cachedCursorsBefore = session->cachedCursors();

    // Expect cursors to be cached.
    {
        auto config = "";
        cursor = session->getCachedCursor(tableId, config);
        ASSERT(cursor);
        session->releaseCursor(tableId, cursor, config);
        ASSERT_EQ(session->cachedCursors(), cachedCursorsBefore);
    }

    // Expect cursors to be cached.
    {
        auto config = "overwrite=false";
        cursor = session->getCachedCursor(tableId, config);
        ASSERT(cursor);
        session->releaseCursor(tableId, cursor, config);
        ASSERT_EQ(session->cachedCursors(), cachedCursorsBefore);
    }

    // Use yet another cursor config, and expect no cursors to be cached.
    {
        auto config = "overwrite=true";
        cursor = session->getCachedCursor(tableId, config);
        ASSERT_FALSE(cursor);

        cursor = session->getNewCursor(uri, config);
        ASSERT(cursor);
        session->releaseCursor(tableId, cursor, config);
        ASSERT_GT(session->cachedCursors(), cachedCursorsBefore);
    }
}

}  // namespace mongo
