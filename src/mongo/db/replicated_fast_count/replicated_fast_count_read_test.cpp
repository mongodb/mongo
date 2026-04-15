/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/replicated_fast_count/replicated_fast_count_read.h"

#include "mongo/db/replicated_fast_count/replicated_fast_count_init.h"
#include "mongo/db/replicated_fast_count/replicated_fast_count_test_helpers.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"

namespace mongo::replicated_fast_count {
namespace {

/**
 * Allows explicit control over the contents of the "oplog" used to aggregate size and count. This
 * test cursor does not have visibility rules specific to the oplog, but should suffice for
 * targeted testing of aggregation logic.
 */
class OplogCursorMock : public SeekableRecordCursor {
public:
    OplogCursorMock(std::list<repl::OplogEntry> entries) {
        for (const auto& entry : entries) {
            _records.emplace_back(RecordId(entry.getTimestamp().asULL()),
                                  entry.getEntry().toBSON().getOwned());
        }
    }

    ~OplogCursorMock() override {}

    boost::optional<Record> next() override {
        if (_records.empty()) {
            return boost::none;
        }

        if (!_initialized) {
            _initialized = true;
            _it = _records.cbegin();
        } else {
            ++_it;
        }

        if (_it == _records.cend()) {
            _initialized = false;
            return boost::none;
        }

        return Record{_it->first, RecordData(_it->second.objdata(), _it->second.objsize())};
    }

    boost::optional<Record> seekExact(const RecordId& id) override {
        for (auto it = _records.cbegin(); it != _records.cend(); ++it) {
            if (it->first == id) {
                _initialized = true;
                _it = it;
                return Record{it->first, RecordData(it->second.objdata(), it->second.objsize())};
            }
        }
        _initialized = false;
        return boost::none;
    }

    void save() override {}
    bool restore(RecoveryUnit&, bool) override {
        return true;
    }
    void detachFromOperationContext() override {}
    void reattachToOperationContext(OperationContext*) override {}
    void setSaveStorageCursorOnDetachFromOperationContext(bool) override {}

    boost::optional<Record> seek(const RecordId& start, BoundInclusion boundInclusion) override {
        invariant(boundInclusion == BoundInclusion::kExclude);
        for (auto it = _records.cbegin(); it != _records.cend(); ++it) {
            if (it->first > start) {
                _initialized = true;
                _it = it;
                return Record{it->first, RecordData(it->second.objdata(), it->second.objsize())};
            }
        }
        _initialized = false;
        return {};
    }

private:
    bool _initialized = false;
    std::list<std::pair<RecordId, BSONObj>> _records;
    std::list<std::pair<RecordId, BSONObj>>::const_iterator _it;
};

class ReadLatestTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
        ASSERT_OK(
            createReplicatedFastCountTimestampCollection(storageInterface(), operationContext()));
    }

    const test_helpers::NsAndUUID collA = {
        .nss = NamespaceString::createNamespaceString_forTest("read_exact", "collA"),
        .uuid = UUID::gen()};
    const test_helpers::NsAndUUID collB = {
        .nss = NamespaceString::createNamespaceString_forTest("read_exact", "collB"),
        .uuid = UUID::gen()};
};

TEST_F(ReadLatestTest, UuidNotFoundInSizeCountStore) {
    const SizeCountStore sizeCountStore;
    const SizeCountTimestampStore timestampStore;
    OplogCursorMock cursor(std::list<repl::OplogEntry>{});
    const UUID uuid = UUID::gen();

    EXPECT_EQ(readLatest(operationContext(), sizeCountStore, timestampStore, cursor, uuid),
              CollectionSizeCount({.size = 0, .count = 0}));
}

TEST_F(ReadLatestTest, UuidNotFoundInSizeCountTimestampStore) {
    SizeCountStore sizeCountStore;
    const SizeCountTimestampStore timestampStore;
    OplogCursorMock cursor(std::list<repl::OplogEntry>{});
    const UUID uuid = UUID::gen();

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       uuid,
                                       {
                                           .timestamp = Timestamp(0, 1),
                                           .size = 1,
                                           .count = 10,
                                       });
    EXPECT_EQ(readLatest(operationContext(), sizeCountStore, timestampStore, cursor, uuid),
              CollectionSizeCount({.size = 1, .count = 10}));
}

TEST_F(ReadLatestTest, EmptyOplog) {
    SizeCountStore sizeCountStore;
    const SizeCountTimestampStore timestampStore;
    OplogCursorMock cursor(std::list<repl::OplogEntry>{});
    const UUID uuid = UUID::gen();

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       uuid,
                                       {.timestamp = Timestamp::min(), .size = 5, .count = 1});
    EXPECT_EQ(readLatest(operationContext(), sizeCountStore, timestampStore, cursor, uuid),
              CollectionSizeCount({.size = 5, .count = 1}));
}

TEST_F(ReadLatestTest, UuidNotFoundInNonEmptyOplog) {
    SizeCountStore sizeCountStore;
    const SizeCountTimestampStore timestampStore;

    const std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/),
    };
    OplogCursorMock cursor(entries);

    // We assume this UUID != collA.uuid, so the oplog entry should be ignored.
    const UUID uuid = UUID::gen();
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       uuid,
                                       {.timestamp = Timestamp::min(), .size = 5, .count = 1});

    EXPECT_EQ(readLatest(operationContext(), sizeCountStore, timestampStore, cursor, uuid),
              CollectionSizeCount({.size = 5, .count = 1}));
}

TEST_F(ReadLatestTest, TimestampNotFoundInNonEmptyOplog) {
    SizeCountStore sizeCountStore;
    SizeCountTimestampStore timestampStore;

    const std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/),
    };
    OplogCursorMock cursor(entries);

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp::min(), .size = 5, .count = 1});

    // Timestamp >= (1, 1) should skip all oplog entries since oplog traversal begins *after* the
    // timestamp store timestamp.
    test_helpers::insertSizeCountTimestamp(operationContext(), timestampStore, Timestamp(1, 1));
    EXPECT_EQ(readLatest(operationContext(), sizeCountStore, timestampStore, cursor, collA.uuid),
              CollectionSizeCount({.size = 5, .count = 1}));
}

TEST_F(ReadLatestTest, UuidFoundInOplog) {
    SizeCountStore sizeCountStore;
    SizeCountTimestampStore timestampStore;

    const std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(
            Timestamp(2, 2), collA, repl::OpTypeEnum::kUpdate, 100 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(
            Timestamp(3, 3), collA, repl::OpTypeEnum::kDelete, -50 /*sizeDelta=*/),
    };
    OplogCursorMock cursor(entries);

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp::min(), .size = 5, .count = 1});
    test_helpers::insertSizeCountTimestamp(operationContext(), timestampStore, Timestamp::min());

    EXPECT_EQ(readLatest(operationContext(), sizeCountStore, timestampStore, cursor, collA.uuid),
              CollectionSizeCount({.size = 5 + 10 + 100 - 50, .count = 1 + 1 + 0 - 1}));
}

TEST_F(ReadLatestTest, UuidFoundInOplogAfterSizeCountStoreTimestamp) {
    SizeCountStore sizeCountStore;
    SizeCountTimestampStore timestampStore;

    const std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(
            Timestamp(2, 2), collA, repl::OpTypeEnum::kUpdate, 100 /*sizeDelta=*/),
    };
    OplogCursorMock cursor(entries);

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp(1, 1), .size = 5, .count = 1});
    test_helpers::insertSizeCountTimestamp(operationContext(), timestampStore, Timestamp::min());

    EXPECT_EQ(readLatest(operationContext(), sizeCountStore, timestampStore, cursor, collA.uuid),
              CollectionSizeCount({.size = 5 + 100, .count = 1}));
}

TEST_F(ReadLatestTest, UuidFoundInOplogAfterTimestampStoreTimestamp) {
    SizeCountStore sizeCountStore;
    SizeCountTimestampStore timestampStore;

    const std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(
            Timestamp(2, 2), collA, repl::OpTypeEnum::kUpdate, 100 /*sizeDelta=*/),
    };
    OplogCursorMock cursor(entries);

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp::min(), .size = 5, .count = 1});
    test_helpers::insertSizeCountTimestamp(operationContext(), timestampStore, Timestamp(1, 1));

    EXPECT_EQ(readLatest(operationContext(), sizeCountStore, timestampStore, cursor, collA.uuid),
              CollectionSizeCount({.size = 5 + 100, .count = 1}));
}

TEST_F(ReadLatestTest, UuidFoundInOplogWithInterleavedEntries) {
    SizeCountStore sizeCountStore;
    SizeCountTimestampStore timestampStore;

    const std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kInsert, 10 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(
            Timestamp(2, 2), collB, repl::OpTypeEnum::kInsert, 200 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(
            Timestamp(3, 3), collA, repl::OpTypeEnum::kUpdate, 100 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(
            Timestamp(4, 4), collB, repl::OpTypeEnum::kDelete, -50 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(
            Timestamp(5, 5), collA, repl::OpTypeEnum::kDelete, -30 /*sizeDelta=*/),
    };
    OplogCursorMock cursor(entries);

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp::min(), .size = 5, .count = 1});
    test_helpers::insertSizeCountTimestamp(operationContext(), timestampStore, Timestamp::min());

    // Only collA's deltas should be aggregated.
    EXPECT_EQ(readLatest(operationContext(), sizeCountStore, timestampStore, cursor, collA.uuid),
              CollectionSizeCount({.size = 5 + 10 + 100 - 30, .count = 1 + 1 + 0 - 1}));
}

TEST_F(ReadLatestTest, OnlyUpdateOpsResultInZeroCountDelta) {
    SizeCountStore sizeCountStore;
    SizeCountTimestampStore timestampStore;

    const std::list<repl::OplogEntry> entries{
        test_helpers::makeOplogEntry(
            Timestamp(1, 1), collA, repl::OpTypeEnum::kUpdate, 10 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(
            Timestamp(2, 2), collA, repl::OpTypeEnum::kUpdate, 20 /*sizeDelta=*/),
        test_helpers::makeOplogEntry(
            Timestamp(3, 3), collA, repl::OpTypeEnum::kUpdate, -5 /*sizeDelta=*/),
    };
    OplogCursorMock cursor(entries);

    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp::min(), .size = 100, .count = 5});
    test_helpers::insertSizeCountTimestamp(operationContext(), timestampStore, Timestamp::min());

    // Updates contribute size deltas but zero count delta, so count stays the same.
    EXPECT_EQ(readLatest(operationContext(), sizeCountStore, timestampStore, cursor, collA.uuid),
              CollectionSizeCount({.size = 100 + 10 + 20 - 5, .count = 5}));
}

class ReadPersistedTest : public CatalogTestFixture {
protected:
    void setUp() override {
        CatalogTestFixture::setUp();
        ASSERT_OK(createReplicatedFastCountCollection(storageInterface(), operationContext()));
    }

    const test_helpers::NsAndUUID collA = {
        .nss = NamespaceString::createNamespaceString_forTest("read_approx", "collA"),
        .uuid = UUID::gen()};
    const test_helpers::NsAndUUID collB = {
        .nss = NamespaceString::createNamespaceString_forTest("read_approx", "collB"),
        .uuid = UUID::gen()};
};

TEST_F(ReadPersistedTest, UuidNotFoundInSizeCountStore) {
    const SizeCountStore sizeCountStore;
    EXPECT_THROW(std::ignore = readPersisted(operationContext(), sizeCountStore, UUID::gen()),
                 DBException);
}

TEST_F(ReadPersistedTest, UuidFoundInSizeCountStore) {
    SizeCountStore sizeCountStore;
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collA.uuid,
                                       {.timestamp = Timestamp(1, 1), .size = 5, .count = 1});
    test_helpers::insertSizeCountEntry(operationContext(),
                                       sizeCountStore,
                                       collB.uuid,
                                       {.timestamp = Timestamp(1, 1), .size = 10, .count = 2});
    EXPECT_EQ(readPersisted(operationContext(), sizeCountStore, collA.uuid),
              CollectionSizeCount({.size = 5, .count = 1}));
}

}  // namespace
}  // namespace mongo::replicated_fast_count
