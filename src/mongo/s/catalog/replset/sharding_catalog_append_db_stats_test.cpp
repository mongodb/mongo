/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/replset/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/replset/sharding_catalog_test_fixture.h"
#include "mongo/stdx/future.h"
#include "mongo/util/log.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

using executor::NetworkInterfaceMock;
using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;

using ShardingCatalogClientAppendDbStatsTest = ShardingCatalogTestFixture;

const BSONObj kReplSecondaryOkMetadata{[] {
    BSONObjBuilder o;
    o.appendElements(rpc::ServerSelectionMetadata(true, boost::none).toBSON());
    o.append(rpc::kReplSetMetadataFieldName, 1);
    return o.obj();
}()};

TEST_F(ShardingCatalogClientAppendDbStatsTest, BasicAppendDBStats) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONArrayBuilder builder;
    auto future = launchAsync([this, &builder] {
        ASSERT_OK(
            catalogClient()->appendInfoForConfigServerDatabases(operationContext(), &builder));
    });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ("admin", request.dbname);
        ASSERT_EQ(BSON("listDatabases" << 1 << "maxTimeMS" << 30000), request.cmdObj);

        return fromjson(R"({
            databases: [
                {
                    name: 'admin',
                    empty: false,
                    sizeOnDisk: 11111
                },
                {
                    name: 'local',
                    empty: false,
                    sizeOnDisk: 33333
                },
                {
                    name: 'config',
                    empty: false,
                    sizeOnDisk: 40000
                }
            ],
            ok: 1
        })");
    });

    future.timed_get(kFutureTimeout);

    BSONArray dbList = builder.arr();
    std::map<std::string, long long> dbMap;
    BSONArrayIteratorSorted iter(dbList);
    while (iter.more()) {
        auto dbEntryObj = iter.next().Obj();
        dbMap[dbEntryObj["name"].String()] = dbEntryObj["sizeOnDisk"].numberLong();
    }

    auto adminIter = dbMap.find("admin");
    ASSERT_TRUE(adminIter != dbMap.end());
    ASSERT_EQ(11111, adminIter->second);

    auto configIter = dbMap.find("config");
    ASSERT_TRUE(configIter != dbMap.end());
    ASSERT_EQ(40000, configIter->second);

    auto localIter = dbMap.find("local");
    ASSERT_TRUE(localIter == dbMap.end());
}

TEST_F(ShardingCatalogClientAppendDbStatsTest, ErrorRunningListDatabases) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONArrayBuilder builder;
    auto future = launchAsync([this, &builder] {
        auto status =
            catalogClient()->appendInfoForConfigServerDatabases(operationContext(), &builder);
        ASSERT_NOT_OK(status);
        ASSERT_EQ(ErrorCodes::AuthenticationFailed, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest&) {
        return Status(ErrorCodes::AuthenticationFailed, "illegal");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientAppendDbStatsTest, MalformedListDatabasesResponse) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONArrayBuilder builder;
    auto future = launchAsync([this, &builder] {
        auto status =
            catalogClient()->appendInfoForConfigServerDatabases(operationContext(), &builder);
        ASSERT_NOT_OK(status);
        ASSERT_EQ(ErrorCodes::NoSuchKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest&) { return BSON("ok" << 1); });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogClientAppendDbStatsTest, MalformedListDatabasesEntryInResponse) {
    configTargeter()->setFindHostReturnValue(HostAndPort("TestHost1"));

    BSONArrayBuilder builder;
    auto future = launchAsync([this, &builder] {
        auto status =
            catalogClient()->appendInfoForConfigServerDatabases(operationContext(), &builder);
        ASSERT_NOT_OK(status);
        ASSERT_EQ(ErrorCodes::NoSuchKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onCommand([](const RemoteCommandRequest&) {
        return fromjson(R"({
            databases: [
                {
                    noname: 'admin',
                    empty: false,
                    sizeOnDisk: 11111
                }
            ],
            ok: 1
        })");
    });

    future.timed_get(kFutureTimeout);
}

}  // unnamed namespace
}  // namespace mongo
