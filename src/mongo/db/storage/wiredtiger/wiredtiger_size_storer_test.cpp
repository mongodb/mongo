/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <wiredtiger.h>

namespace mongo {
namespace {

WT_CONNECTION* openConnection(const unittest::TempDir& tempDir) {
    WT_CONNECTION* conn;
    ASSERT_OK(
        wtRCToStatus(wiredtiger_open(tempDir.path().c_str(), nullptr, "create", &conn), nullptr));
    return conn;
}

class WiredTigerSizeStorerTest : public ServiceContextTest {
protected:
    WiredTigerSizeStorerTest()
        : _conn(openConnection(_tempDir), &_clockSource, /*sessionCacheMax=*/33000),
          _session(&_conn) {}

    WiredTigerSizeStorer makeSizeStorer() {
        return {&_conn, "table:sizeStorer"};
    }

private:
    unittest::TempDir _tempDir{"WiredTigerSizeStorerTest"};
    ClockSourceMock _clockSource;
    WiredTigerConnection _conn;

protected:
    WiredTigerSession _session;
};

TEST_F(WiredTigerSizeStorerTest, Store) {
    auto sizeStorer1 = makeSizeStorer();
    auto sizeStorer2 = makeSizeStorer();
    auto sizeInfo = std::make_shared<WiredTigerSizeStorer::SizeInfo>(1, 10);
    StringData uri{"uri1"};

    sizeStorer1.store(uri, sizeInfo);

    auto loaded = sizeStorer1.load(_session, uri);
    ASSERT(loaded);
    ASSERT_EQ(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), sizeInfo->numRecords.load());
    ASSERT_EQ(loaded->dataSize.load(), sizeInfo->dataSize.load());

    loaded = sizeStorer2.load(_session, uri);
    ASSERT(loaded);
    ASSERT_NE(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), 0);
    ASSERT_EQ(loaded->dataSize.load(), 0);

    sizeStorer1.flush(false);

    loaded = sizeStorer1.load(_session, uri);
    ASSERT(loaded);
    ASSERT_NE(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), sizeInfo->numRecords.load());
    ASSERT_EQ(loaded->dataSize.load(), sizeInfo->dataSize.load());

    loaded = sizeStorer2.load(_session, uri);
    ASSERT(loaded);
    ASSERT_NE(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), sizeInfo->numRecords.load());
    ASSERT_EQ(loaded->dataSize.load(), sizeInfo->dataSize.load());
}

TEST_F(WiredTigerSizeStorerTest, RemoveBeforeFlush) {
    auto sizeStorer = makeSizeStorer();
    auto sizeInfo = std::make_shared<WiredTigerSizeStorer::SizeInfo>(1, 10);
    StringData uri{"uri1"};

    sizeStorer.store(uri, sizeInfo);

    auto loaded = sizeStorer.load(_session, uri);
    ASSERT(loaded);
    ASSERT_EQ(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), sizeInfo->numRecords.load());
    ASSERT_EQ(loaded->dataSize.load(), sizeInfo->dataSize.load());

    sizeStorer.remove(uri);

    loaded = sizeStorer.load(_session, uri);
    ASSERT(loaded);
    ASSERT_NE(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), 0);
    ASSERT_EQ(loaded->dataSize.load(), 0);

    sizeStorer.flush(false);

    loaded = sizeStorer.load(_session, uri);
    ASSERT(loaded);
    ASSERT_NE(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), 0);
    ASSERT_EQ(loaded->dataSize.load(), 0);
}

TEST_F(WiredTigerSizeStorerTest, RemoveAfterFlush) {
    auto sizeStorer = makeSizeStorer();
    auto sizeInfo = std::make_shared<WiredTigerSizeStorer::SizeInfo>(1, 10);
    StringData uri{"uri1"};

    sizeStorer.store(uri, sizeInfo);
    sizeStorer.flush(false);

    auto loaded = sizeStorer.load(_session, uri);
    ASSERT(loaded);
    ASSERT_NE(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), sizeInfo->numRecords.load());
    ASSERT_EQ(loaded->dataSize.load(), sizeInfo->dataSize.load());

    sizeStorer.remove(uri);

    loaded = sizeStorer.load(_session, uri);
    ASSERT(loaded);
    ASSERT_NE(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), 0);
    ASSERT_EQ(loaded->dataSize.load(), 0);

    sizeStorer.flush(false);

    loaded = sizeStorer.load(_session, uri);
    ASSERT(loaded);
    ASSERT_NE(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), 0);
    ASSERT_EQ(loaded->dataSize.load(), 0);
}

TEST_F(WiredTigerSizeStorerTest, RemoveNonexistent) {
    auto sizeStorer = makeSizeStorer();
    auto sizeInfo = std::make_shared<WiredTigerSizeStorer::SizeInfo>(1, 10);
    StringData uri{"uri1"};

    sizeStorer.remove(uri);

    auto loaded = sizeStorer.load(_session, uri);
    ASSERT(loaded);
    ASSERT_NE(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), 0);
    ASSERT_EQ(loaded->dataSize.load(), 0);

    sizeStorer.store(uri, sizeInfo);

    loaded = sizeStorer.load(_session, uri);
    ASSERT(loaded);
    ASSERT_EQ(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), sizeInfo->numRecords.load());
    ASSERT_EQ(loaded->dataSize.load(), sizeInfo->dataSize.load());

    sizeStorer.flush(false);

    loaded = sizeStorer.load(_session, uri);
    ASSERT(loaded);
    ASSERT_NE(loaded, sizeInfo);
    ASSERT_EQ(loaded->numRecords.load(), sizeInfo->numRecords.load());
    ASSERT_EQ(loaded->dataSize.load(), sizeInfo->dataSize.load());
}

}  // namespace
}  // namespace mongo
