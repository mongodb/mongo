// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/storage/wiredtiger/wiredtiger_size_storer.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_connection.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_error_util.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"

#include <string_view>

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
    std::string_view uri{"uri1"};

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
    std::string_view uri{"uri1"};

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
    std::string_view uri{"uri1"};

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
    std::string_view uri{"uri1"};

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
