// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

TEST(WiredTigerSessionTest, StaleCursorNotReturnedToCacheAfterRollbackToStable) {
    WiredTigerHarnessHelper helper;
    std::unique_ptr<RecordStore> rs = helper.newRecordStore();
    auto uri = std::string{static_cast<WiredTigerRecordStore*>(rs.get())->getURI()};
    auto& conn = helper.connection();

    std::unique_ptr<RecoveryUnit> ruA = helper.newRecoveryUnit();
    auto* session = static_cast<WiredTigerRecoveryUnit*>(ruA.get())->getSession();

    // Populate cache before epoch bump
    {
        session->closeAllCursors(uri);
        ASSERT_EQ(session->cachedCursors(), 0);

        WT_CURSOR* cursor = session->getNewCursor(uri, "");
        ASSERT(cursor);
        session->releaseCursor(999999999, cursor, "");
        ASSERT_GT(session->cachedCursors(), 0);
    }

    // Bump RTS epoch
    conn.closeAll(WiredTigerConnection::ShutdownReason::kRollbackToStable);

    // Check that the stale cursor was closed and not returned to the cache
    {
        WT_CURSOR* cursor = session->getCachedCursor(999999999, "");
        ASSERT(cursor);
        session->releaseCursor(999999999, cursor, "");
        ASSERT_EQ(session->cachedCursors(), 0);

        WT_CURSOR* staleCursor = session->getCachedCursor(999999999, "");
        ASSERT_FALSE(staleCursor) << "Stale cursor should have been closed after RTS epoch bump";
    }
}

TEST(WiredTigerSessionTest, CursorNotCachedAfterCleanShutdown) {
    WiredTigerHarnessHelper helper;
    auto& conn = helper.connection();
    std::unique_ptr<RecordStore> rs = helper.newRecordStore();
    auto uri = std::string{static_cast<WiredTigerRecordStore*>(rs.get())->getURI()};

    std::unique_ptr<RecoveryUnit> ru = helper.newRecoveryUnit();
    auto* session = static_cast<WiredTigerRecoveryUnit*>(ru.get())->getSession();
    session->closeAllCursors(uri);
    ASSERT_EQ(session->cachedCursors(), 0);

    WT_CURSOR* cursor = session->getNewCursor(uri, "");
    ASSERT(cursor);

    int cachedBefore = session->cachedCursors();

    // Trigger CleanShutdown to bump engine epoch
    conn.closeAll(WiredTigerConnection::ShutdownReason::kCleanShutdown);

    // CleanShutdown path in releaseCursor should early-return
    session->releaseCursor(555555, cursor, "");
    ASSERT_EQ(session->cachedCursors(), cachedBefore);

    // CHeck that the cursor was not cached
    WT_CURSOR* checkCursor = session->getCachedCursor(555555, "");
    ASSERT_FALSE(checkCursor)
        << "Cursor should not be cached after clean-shutdown engine epoch bump";
}

TEST(WiredTigerSessionTest, VerifyConfig) {
    WiredTigerHarnessHelper helper;
    std::unique_ptr<RecoveryUnit> ru = helper.newRecoveryUnit();
    std::unique_ptr<RecordStore> rs = helper.newRecordStore();
    auto uri = std::string{static_cast<WiredTigerRecordStore*>(rs.get())->getURI()};

    auto* session = static_cast<WiredTigerRecoveryUnit*>(ru.get())->getSession();

    // Verify requires zero open cursors on the table.
    session->closeAllCursors(uri);

    ASSERT_EQ(0, session->verify(uri.c_str(), nullptr));
    ASSERT_EQ(0, session->verify(uri.c_str(), ""));
    ASSERT_EQ(0, session->verify(uri.c_str(), "skip_per_key_hs=false"));
}

}  // namespace mongo
