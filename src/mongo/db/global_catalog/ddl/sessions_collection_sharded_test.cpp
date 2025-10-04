/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "cxxabi.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/global_catalog/catalog_cache/catalog_cache_test_fixture.h"
#include "mongo/db/global_catalog/ddl/sessions_collection_sharded.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <system_error>
#include <utility>

namespace mongo {
namespace {

using executor::RemoteCommandRequest;

LogicalSessionRecord makeRecord(Date_t time = Date_t::now()) {
    auto record = makeLogicalSessionRecordForTest();
    record.setLastUse(time);
    return record;
}

/**
 * Mimics a two shards backend.
 */
class SessionsCollectionShardedTest : public RouterCatalogCacheTestFixture {
public:
    SessionsCollectionShardedTest() = default;
    ~SessionsCollectionShardedTest() override = default;

    void setUp() override {
        RouterCatalogCacheTestFixture::setUp();

        _shards = {std::move(setupNShards(2))};
    }

    SessionsCollectionSharded _collection;
    std::vector<ShardType> _shards;
};

TEST_F(SessionsCollectionShardedTest, RefreshOneSessionOKTest) {
    // Set up routing table for the logical sessions collection.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(NamespaceString::kLogicalSessionsNamespace,
                                                  BSON("_id" << 1));
    auto future = launchAsync([&] {
        auto now = Date_t::now();
        auto thePast = now - Minutes(5);

        auto record1 = makeRecord(thePast);
        _collection.refreshSessions(operationContext(), {record1});
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(1);

        return response.toBSON();
    });

    future.default_timed_get();
}

TEST_F(SessionsCollectionShardedTest, CheckReadConcern) {
    // Set up routing table for the logical sessions collection.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(NamespaceString::kLogicalSessionsNamespace,
                                                  BSON("_id" << 1));
    auto future = launchAsync([&] {
        auto notInsertedRecord = makeRecord();
        LogicalSessionIdSet lsids{notInsertedRecord.getId()};

        _collection.findRemovedSessions(operationContext(), lsids);
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        BSONObj obj = request.cmdObj;
        auto readConcern = obj.getObjectField("readConcern");
        ASSERT_FALSE(readConcern.isEmpty());
        auto level = readConcern.getStringField("level");
        ASSERT_EQ(level, "local");

        return CursorResponse().toBSONAsInitialResponse();
    });

    future.default_timed_get();
}

TEST_F(SessionsCollectionShardedTest, RefreshOneSessionStatusErrTest) {
    // Set up routing table for the logical sessions collection.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(NamespaceString::kLogicalSessionsNamespace,
                                                  BSON("_id" << 1));
    auto future = launchAsync([&] {
        auto now = Date_t::now();
        auto thePast = now - Minutes(5);

        auto record1 = makeRecord(thePast);
        _collection.refreshSessions(operationContext(), {record1});
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        return Status(ErrorCodes::BSONObjectTooLarge, "BSON size limit hit while parsing message");
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::BSONObjectTooLarge);
}

TEST_F(SessionsCollectionShardedTest, RefreshOneSessionWriteErrTest) {
    // Set up routing table for the logical sessions collection.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(NamespaceString::kLogicalSessionsNamespace,
                                                  BSON("_id" << 1));
    auto future = launchAsync([&] {
        auto now = Date_t::now();
        auto thePast = now - Minutes(5);

        auto record1 = makeRecord(thePast);
        _collection.refreshSessions(operationContext(), {record1});
    });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails(
            write_ops::WriteError(0, {ErrorCodes::NotWritablePrimary, "not primary"}));
        return response.toBSON();
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::NotWritablePrimary);
}

TEST_F(SessionsCollectionShardedTest, RemoveOneSessionOKTest) {
    // Set up routing table for the logical sessions collection.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(NamespaceString::kLogicalSessionsNamespace,
                                                  BSON("_id" << 1));
    auto future = launchAsync(
        [&] { _collection.removeRecords(operationContext(), {makeLogicalSessionIdForTest()}); });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.setNModified(1);
        return response.toBSON();
    });

    future.default_timed_get();
}

TEST_F(SessionsCollectionShardedTest, RemoveOneSessionStatusErrTest) {
    // Set up routing table for the logical sessions collection.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(NamespaceString::kLogicalSessionsNamespace,
                                                  BSON("_id" << 1));
    auto future = launchAsync(
        [&] { _collection.removeRecords(operationContext(), {makeLogicalSessionIdForTest()}); });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        return Status(ErrorCodes::BSONObjectTooLarge, "BSON size limit hit while parsing message");
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::BSONObjectTooLarge);
}

TEST_F(SessionsCollectionShardedTest, RemoveOneSessionWriteErrTest) {
    // Set up routing table for the logical sessions collection.
    loadRoutingTableWithTwoChunksAndTwoShardsImpl(NamespaceString::kLogicalSessionsNamespace,
                                                  BSON("_id" << 1));
    auto future = launchAsync(
        [&] { _collection.removeRecords(operationContext(), {makeLogicalSessionIdForTest()}); });

    onCommandForPoolExecutor([&](const RemoteCommandRequest& request) {
        BatchedCommandResponse response;
        response.setStatus(Status::OK());
        response.setNModified(0);
        response.addToErrDetails(
            write_ops::WriteError(0, {ErrorCodes::NotWritablePrimary, "not primary"}));
        return response.toBSON();
    });

    ASSERT_THROWS_CODE(future.default_timed_get(), DBException, ErrorCodes::NotWritablePrimary);
}

}  // namespace
}  // namespace mongo
