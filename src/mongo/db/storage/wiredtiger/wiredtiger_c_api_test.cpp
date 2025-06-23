/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

#include <cstddef>
#include <ostream>
#include <string>

#include <wiredtiger.h>

#include <boost/filesystem/directory.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

/**
 * This suite holds test cases that run against the WiredTiger C API (wiredtiger.h) directly.
 * Some of these tests may have counterparts in the C/Python tests shipped with the WiredTiger
 * distribution.  For example, RollbackToStable40 is derived from test_rollback_to_stable40.py
 * in third_party/wiredtiger.
 *
 * The intent of this suite is to provide a template for writing WiredTiger tests with MongoDB
 * test facilities and help stage unit tests that will eventually be converted to run against
 * a higher level interface such as the KVEngine/RecordStore interface or the MongoDB collection
 * catalog.
 */

// See WT-9870 and src/third_party//wiredtiger/test/suite/test_rollback_to_stable40.py.
TEST(WiredTigerCApiTest, RollbackToStable40) {
    WT_CONNECTION* conn = nullptr;
    WT_SESSION* session = nullptr;
    WT_CURSOR* cursor = nullptr;
    WT_CURSOR* evict_cursor = nullptr;

    int nrows = 3;
    std::string valueA(500, 'a');
    std::string valueB(500, 'b');
    std::string valueC(500, 'c');
    std::string valueD(500, 'd');

    // Open the connection.
    unittest::TempDir home("WiredTigerRollbackToStable40Test_home");
    const std::string connection_cfg("create,cache_size=1MB,statistics=(all),log=(enabled=true)");
    auto ret = wiredtiger_open(home.path().c_str(), nullptr, connection_cfg.c_str(), &conn);
    ASSERT_OK(wtRCToStatus(ret, /*session=*/nullptr)) << fmt::format(
        "failed to open connection to source folder {} with {}", home.path(), connection_cfg);

    // Open a session.
    const std::string session_cfg("isolation=snapshot");
    ret = conn->open_session(conn, nullptr, session_cfg.c_str(), &session);
    ASSERT_OK(wtRCToStatus(ret, /*session=*/nullptr))
        << "failed to create session with config isolation=snapshot";

    // Create a table without logging.
    const std::string uri("table:rollback_to_stable40");
    const std::string table_cfg("key_format=i,value_format=S,log=(enabled=false)");
    ret = session->create(session, uri.c_str(), table_cfg.c_str());
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to create table {} with config {}", uri, table_cfg);

    // Pin oldest and stable to timestamps 10.
    ret = conn->set_timestamp(
        conn, fmt::format("oldest_timestamp={:x},stable_timestamp={:x}", 10, 10).c_str());
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to set oldest and stable timestamps to {} (hex: {:x})", 10, 10);

    ret = session->open_cursor(session, uri.c_str(), nullptr, nullptr, &cursor);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to open cursor to insert initial set of keys";

    // Insert 3 keys with the value A.
    ret = session->begin_transaction(session, nullptr);
    ASSERT_OK(wtRCToStatus(ret, session)) << fmt::format(
        "failed to start transaction to insert initial set of 3 keys with value {}", valueA);

    cursor->set_key(cursor, 1);
    cursor->set_value(cursor, valueA.c_str());
    ret = cursor->insert(cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to insert first key {} with value {}", 1, valueA);

    cursor->set_key(cursor, 2);
    cursor->set_value(cursor, valueA.c_str());
    ret = cursor->insert(cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to insert middle key {} with value {}", 2, valueA);

    cursor->set_key(cursor, 3);
    cursor->set_value(cursor, valueA.c_str());
    ret = cursor->insert(cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to insert last key {} with value {}", 3, valueA);

    ret = session->commit_transaction(session, fmt::format("commit_timestamp={:x}", 20).c_str());
    ASSERT_OK(wtRCToStatus(ret, session)) << fmt::format(
        "failed to commit transaction after updating first and last keys with timestamp {} (hex: "
        "{:x})",
        20,
        20);

    // Update the first and last keys with another value with a large timestamp.
    ret = session->begin_transaction(session, nullptr);
    ASSERT_OK(wtRCToStatus(ret, session))
        << "failed to start transaction to update first and last keys";

    cursor->set_key(cursor, 1);
    cursor->set_value(cursor, valueD.c_str());
    ret = cursor->insert(cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to update first key {} with value {}", 1, valueD);

    cursor->set_key(cursor, 3);
    cursor->set_value(cursor, valueD.c_str());
    ret = cursor->insert(cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to update last key {} with value {}", 3, valueD);

    ret = session->commit_transaction(session, fmt::format("commit_timestamp={:x}", 1000).c_str());
    ASSERT_OK(wtRCToStatus(ret, session)) << fmt::format(
        "failed to commit transaction after updating first and last keys with timestamp {} (hex: "
        "{:x})",
        1000,
        1000);

    // Update the middle key with lots of updates to generate more history.
    for (int i = 21; i < 499; ++i) {
        ret = session->begin_transaction(session, nullptr);
        ASSERT_OK(wtRCToStatus(ret, session)) << fmt::format(
            "failed to start transaction for updating middle key (update counter: {})", i);

        auto valueBWithISuffix = valueB + std::to_string(i);
        cursor->set_key(cursor, 2);
        cursor->set_value(cursor, valueBWithISuffix.c_str());
        ret = cursor->insert(cursor);
        ASSERT_OK(wtRCToStatus(ret, session))
            << fmt::format("failed to updating middle key {} with value {} (update counter: {})",
                           2,
                           valueBWithISuffix,
                           i);

        ret = session->commit_transaction(session, fmt::format("commit_timestamp={:x}", i).c_str());
        ASSERT_OK(wtRCToStatus(ret, session)) << fmt::format(
            "failed to commit transaction for updating middle key (update counter: {} (hex: {:x}))",
            i,
            i);
    }

    // With this checkpoint, all the updates in the history store are persisted to disk.
    ret = session->checkpoint(session, nullptr);
    ASSERT_OK(wtRCToStatus(ret, session))
        << "failed checkpoint after making lots of updates to middle key";

    // Update the middle key with value C.
    ret = session->begin_transaction(session, nullptr);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to start transaction after taking checkpoint";

    cursor->set_key(cursor, 2);
    cursor->set_value(cursor, valueC.c_str());
    ret = cursor->insert(cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to insert record with key {} and value {}", 2, valueC);

    ret = session->commit_transaction(session, fmt::format("commit_timestamp={:x}", 500).c_str());
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to commit transaction with timestamp {} (hex: {:x})", 500, 500);

    // Pin oldest and stable to timestamp 500.
    ret = conn->set_timestamp(
        conn, fmt::format("oldest_timestamp={:x},stable_timestamp={:x}", 500, 500).c_str());
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to set oldest and stable timestamps to {} (hex: {:x})", 500, 500);

    // Evict the globally visible update to write to the disk, this will reset the time window.
    ret =
        session->open_cursor(session, uri.c_str(), nullptr, "debug=(release_evict)", &evict_cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << "failed to open evict cursor with config debug=(release_evict)";
    ret = session->begin_transaction(session, "ignore_prepare=true");
    ASSERT_OK(wtRCToStatus(ret, session))
        << "failed to start transaction using evict cursor with config ignore_prepare=true";

    evict_cursor->set_key(evict_cursor, 2);
    ret = evict_cursor->search(evict_cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to find record using evict cursor with key: {}", 2);

    // Verify the data.
    const char* val;
    evict_cursor->get_value(evict_cursor, &val);
    ASSERT_EQ(std::string(val), valueC)
        << fmt::format("unexpected value in record from evict cursor with key: {}", 2);

    ret = evict_cursor->reset(evict_cursor);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to reset evict cursor on source folder";
    ret = evict_cursor->close(evict_cursor);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to close evict cursor on source folder";
    evict_cursor = nullptr;
    ret = session->rollback_transaction(session, nullptr);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to rollback transaction on source folder";

    // Update middle key with value D.
    ret = session->begin_transaction(session, nullptr);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to start transaction on source folder";

    cursor->set_key(cursor, 2);
    cursor->set_value(cursor, valueD.c_str());
    ret = cursor->insert(cursor);
    ASSERT_OK(wtRCToStatus(ret, session)) << fmt::format(
        "failed to insert record with key {} and value {} on source folder", 2, valueD);

    ret = session->commit_transaction(session, fmt::format("commit_timestamp={:x}", 501).c_str());
    ASSERT_OK(wtRCToStatus(ret, session)) << fmt::format(
        "failed to commit transaction on source folder with timestamp {} (hex: {:x})", 501, 501);

    // 1. This checkpoint will move the globally visible update to the first of the key range.
    // 2. The existing updates in the history store are having with a larger timestamp are
    //    obsolete, so they are not explicitly removed.
    // 3. Any of the history store updates that are already evicted will not rewrite by the
    //    checkpoint.
    ret = session->checkpoint(session, nullptr);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to take checkpoint on source folder";

    // Verify data is visible and correct.
    ret = session->begin_transaction(session, fmt::format("read_timestamp={:x}", 1000).c_str());
    ASSERT_OK(wtRCToStatus(ret, session)) << fmt::format(
        "failed to start transaction on source folder with read timestamp {} (hex: {:x})",
        1000,
        1000);
    for (int i = 1; i < nrows + 1; ++i) {
        cursor->set_key(cursor, i);
        ret = cursor->search(cursor);
        ASSERT_OK(wtRCToStatus(ret, session)) << "failed to find record with key: " << i;
        cursor->get_value(cursor, &val);
        ASSERT_EQ(std::string(val), valueD) << "unexpected value in record with key: " << i;
    }
    ret = session->rollback_transaction(session, nullptr);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to rollback transaction on source folder";

    ret = cursor->close(cursor);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to close cursor on source folder";
    cursor = nullptr;

    // Simulate crash and restart. To do this, copy all the current files in the a different folder,
    // close the current connection and open a new one from the new folder.
    unittest::TempDir destination("WiredTigerRollbackToStable40Test_restart");

    // Iterate through the source directory to copy all the files.
    std::size_t numFilesCopied = 0;
    for (boost::filesystem::directory_iterator file(home.path());
         file != boost::filesystem::directory_iterator();
         ++file) {
        boost::filesystem::path current(file->path());

        // There are files we don't need to copy.
        const std::string current_str(current.filename().string());
        if (current_str.find(std::string("WiredTiger.lock")) != std::string::npos) {
            continue;
        }
        if (current_str.find(std::string("WiredTigerTmplog")) != std::string::npos) {
            continue;
        }
        if (current_str.find(std::string("WiredTigerPreplog")) != std::string::npos) {
            continue;
        }

        ASSERT(boost::filesystem::copy_file(current, destination.path() / current.filename()))
            << fmt::format("failed to copy file to destination folder: {}",
                           current.filename().string());
        numFilesCopied++;
    }
    LOGV2(6982600,
          "copied files from source folder to destination",
          "source"_attr = home.path(),
          "destination"_attr = destination.path(),
          "numFilesCopied"_attr = numFilesCopied);

    // Close the initial connection.
    ret = conn->close(conn, nullptr);
    ASSERT_OK(wtRCToStatus(ret, /*session=*/nullptr))
        << "failed to close connection to source folder";
    conn = nullptr;

    // Open the connection from the destination folder.
    ret = wiredtiger_open(destination.path().c_str(), nullptr, connection_cfg.c_str(), &conn);
    ASSERT_OK(wtRCToStatus(ret, /*session=*/nullptr))
        << "failed to open connection to destination folder";

    ret = conn->open_session(conn, nullptr, session_cfg.c_str(), &session);
    ASSERT_OK(wtRCToStatus(ret, /*session=*/nullptr))
        << "failed to open session to destination folder";

    // Verify data is visible and correct.
    ret = session->open_cursor(session, uri.c_str(), nullptr, nullptr, &cursor);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to open cursor in destination folder";

    ret = session->begin_transaction(session, fmt::format("read_timestamp={:x}", 1000).c_str());
    ASSERT_OK(wtRCToStatus(ret, session)) << fmt::format(
        "failed to start transaction with read timestamp {} (hex: {:x})", 1000, 1000);

    for (int i = 1; i < nrows + 1; ++i) {
        cursor->set_key(cursor, i);
        // Without WT-9334 and WT-9870, this call fails.
        ret = cursor->search(cursor);
        ASSERT_OK(wtRCToStatus(ret, session)) << "failed to find record with key: " << i;
        cursor->get_value(cursor, &val);
        ASSERT_EQ(std::string(val), (i % 2 == 0 ? valueC : valueA))
            << "unexpected value in record with key: " << i;
    }
    ret = session->rollback_transaction(session, nullptr);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to rollback transaction";

    ret = conn->close(conn, nullptr);
    ASSERT_OK(wtRCToStatus(ret, /*session=*/nullptr))
        << "failed to close connection to destination folder";
    conn = nullptr;
}

TEST(WiredTigerCApiTest, DropIdent) {
    WT_CONNECTION* conn = nullptr;
    WT_SESSION* session = nullptr;
    WT_CURSOR* cursor = nullptr;

    std::string valueA(500, 'a');
    std::string valueB(500, 'b');
    std::string valueC(500, 'c');
    std::string valueD(500, 'd');

    // Open the connection.
    unittest::TempDir home("WiredTigerDropIdentTest_home");
    const std::string connection_cfg("create,in_memory=false,cache_size=1MB,statistics=(all)");
    auto ret = wiredtiger_open(home.path().c_str(), nullptr, connection_cfg.c_str(), &conn);
    ASSERT_OK(wtRCToStatus(ret, /*session=*/nullptr)) << fmt::format(
        "failed to open connection to source folder {} with {}", home.path(), connection_cfg);

    // Open a session.
    const std::string session_cfg("isolation=snapshot");
    ret = conn->open_session(conn, nullptr, session_cfg.c_str(), &session);
    ASSERT_OK(wtRCToStatus(ret, /*session=*/nullptr))
        << "failed to create session with config isolation=snapshot";

    // Create a table without logging.
    const std::string uri("table:rollback_to_stable40");
    const std::string table_cfg("key_format=i,value_format=S,log=(enabled=false)");
    ret = session->create(session, uri.c_str(), table_cfg.c_str());
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to create table {} with config {}", uri, table_cfg);

    ret = session->open_cursor(session, uri.c_str(), nullptr, nullptr, &cursor);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to open cursor to insert initial set of keys";

    // Insert 3 keys.
    cursor->set_key(cursor, 1);
    cursor->set_value(cursor, valueA.c_str());
    ret = cursor->insert(cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to insert first key {} with value {}", 1, valueA);

    cursor->set_key(cursor, 2);
    cursor->set_value(cursor, valueB.c_str());
    ret = cursor->insert(cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to insert middle key {} with value {}", 2, valueA);

    cursor->set_key(cursor, 3);
    cursor->set_value(cursor, valueC.c_str());
    ret = cursor->insert(cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to insert last key {} with value {}", 3, valueA);

    cursor->set_key(cursor, 2);
    ret = cursor->search(cursor);
    ASSERT_OK(wtRCToStatus(ret, session))
        << fmt::format("failed to find record using evict cursor with key: {}", 2);

    // Verify the data.
    const char* val;
    cursor->get_value(cursor, &val);
    ASSERT_EQ(std::string(val), valueB)
        << fmt::format("unexpected value in record from evict cursor with key: {}", 2);

    ret = cursor->close(cursor);
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to close cursor on source folder";
    cursor = nullptr;

    ret = session->drop(session, uri.c_str(), "force=true");
    ASSERT_OK(wtRCToStatus(ret, session)) << "failed to drop ident";

    // Close the initial connection.
    ret = conn->close(conn, nullptr);
    ASSERT_OK(wtRCToStatus(ret, /*session=*/nullptr))
        << "failed to close connection to source folder";
    conn = nullptr;
}

}  // namespace mongo
