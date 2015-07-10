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

#include "mongo/platform/basic.h"

#include <vector>

#include "mongo/bson/json.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set.h"
#include "mongo/s/catalog/replset/catalog_manager_replica_set_test_fixture.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_update_request.h"

namespace mongo {
namespace {

using std::vector;

using CatalogManagerReplSetTest = CatalogManagerReplSetTestFixture;

TEST_F(CatalogManagerReplSetTestFixture, UpgradeNotNeeded) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] { ASSERT_OK(catalogManager()->checkAndUpgrade(true)); });

    onFindCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);
        ASSERT_EQ(BSON("find"
                       << "version"),
                  request.cmdObj);

        BSONObj versionDoc(fromjson(R"({
                _id: 1,
                minCompatibleVersion: 6,
                currentVersion: 7,
                clusterId: ObjectId("55919cc6dbe86ce7ac056427")
            })"));

        return vector<BSONObj>{versionDoc};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(CatalogManagerReplSetTestFixture, UpgradeTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "Bad test network"});

    auto future = launchAsync([this] {
        auto status = catalogManager()->checkAndUpgrade(true);
        ASSERT_EQ(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(CatalogManagerReplSetTestFixture, UpgradeClusterMultiVersion) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogManager()->checkAndUpgrade(true);
        ASSERT_EQ(ErrorCodes::RemoteValidationError, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) {

        BSONObj versionDoc(fromjson(R"({
                _id: 1,
                minCompatibleVersion: 2,
                currentVersion: 3,
                clusterId: ObjectId("55919cc6dbe86ce7ac056427")
            })"));

        BSONObj versionDoc2(fromjson(R"({
                _id: 2,
                minCompatibleVersion: 3,
                currentVersion: 4,
                clusterId: ObjectId("55919cc6dbe86ce7ac056427")
            })"));

        return vector<BSONObj>{versionDoc, versionDoc2};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(CatalogManagerReplSetTestFixture, UpgradeInvalidConfigVersionDoc) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogManager()->checkAndUpgrade(true);
        ASSERT_EQ(ErrorCodes::UnsupportedFormat, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) {
        BSONObj versionDoc(fromjson(R"({
                _id: 1,
                minCompatibleVersion: "should be numeric",
                currentVersion: 7,
                clusterId: ObjectId("55919cc6dbe86ce7ac056427")
            })"));

        return vector<BSONObj>{versionDoc};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(CatalogManagerReplSetTestFixture, UpgradeNoVersionDocEmptyConfig) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] { ASSERT_OK(catalogManager()->checkAndUpgrade(true)); });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("admin", request.dbname);
        ASSERT_EQ(BSON("listDatabases" << 1), request.cmdObj);

        return fromjson(R"({
                    databases: [
                        { name: "local" }
                    ],
                    totalSize: 12,
                    ok: 1
                })");
    });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(VersionType::ConfigNS, actualBatchedUpdate.getNS().ns());

        auto updates = actualBatchedUpdate.getUpdates();
        ASSERT_EQUALS(1U, updates.size());
        auto update = updates.front();

        ASSERT_EQUALS(update->getQuery(), update->getUpdateExpr());
        ASSERT_TRUE(update->getUpsert());
        ASSERT_FALSE(update->getMulti());

        VersionType versionDoc;
        ASSERT_TRUE(versionDoc.parseBSON(update->getUpdateExpr(), &errmsg));

        ASSERT_EQ(CURRENT_CONFIG_VERSION, versionDoc.getCurrentVersion());
        ASSERT_EQ(CURRENT_CONFIG_VERSION, versionDoc.getMinCompatibleVersion());
        ASSERT_TRUE(versionDoc.isClusterIdSet());
        ASSERT_FALSE(versionDoc.isExcludingMongoVersionsSet());
        ASSERT_FALSE(versionDoc.isUpgradeIdSet());
        ASSERT_FALSE(versionDoc.isUpgradeStateSet());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(CatalogManagerReplSetTestFixture, UpgradeNoVersionDocEmptyConfigWithAdmin) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] { ASSERT_OK(catalogManager()->checkAndUpgrade(true)); });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("admin", request.dbname);
        ASSERT_EQ(BSON("listDatabases" << 1), request.cmdObj);

        return fromjson(R"({
                    databases: [
                        { name: "local" },
                        { name: "admin" }
                    ],
                    totalSize: 12,
                    ok: 1
                })");
    });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        BatchedUpdateRequest actualBatchedUpdate;
        std::string errmsg;
        ASSERT_TRUE(actualBatchedUpdate.parseBSON(request.dbname, request.cmdObj, &errmsg));
        ASSERT_EQUALS(VersionType::ConfigNS, actualBatchedUpdate.getNS().ns());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(CatalogManagerReplSetTestFixture, UpgradeWriteError) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogManager()->checkAndUpgrade(true);
        ASSERT_EQ(ErrorCodes::DuplicateKey, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("admin", request.dbname);
        ASSERT_EQ(BSON("listDatabases" << 1), request.cmdObj);

        return fromjson(R"({
                    databases: [
                        { name: "local" }
                    ],
                    totalSize: 12,
                    ok: 1
                })");
    });

    onCommand([](const RemoteCommandRequest& request) {
        return fromjson(R"({
                ok: 1,
                nModified: 0,
                n: 0,
                writeErrors: [{
                    index: 0,
                    code: 11000,
                    errmsg: "E11000 duplicate key error index: test.user.$_id_ dup key: { : 1.0 }"
                }]
            })");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(CatalogManagerReplSetTestFixture, UpgradeNoVersionDocNonEmptyConfigServer) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogManager()->checkAndUpgrade(true);
        ASSERT_EQ(ErrorCodes::IncompatibleShardingConfigVersion, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("admin", request.dbname);
        ASSERT_EQ(BSON("listDatabases" << 1), request.cmdObj);

        return fromjson(R"({
                    databases: [
                        { name: "local" },
                        { name: "config" }
                    ],
                    totalSize: 12,
                    ok: 1
                })");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(CatalogManagerReplSetTestFixture, UpgradeTooOld) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogManager()->checkAndUpgrade(true);
        ASSERT_EQ(ErrorCodes::IncompatibleShardingConfigVersion, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) {
        BatchedCommandResponse response;
        response.setOk(true);
        response.setNModified(1);

        BSONObj versionDoc(fromjson(R"({
                _id: 1,
                minCompatibleVersion: 2000000000,
                currentVersion: 2000000000,
                clusterId: ObjectId("55919cc6dbe86ce7ac056427")
            })"));

        return vector<BSONObj>{versionDoc};
    });

    future.timed_get(kFutureTimeout);
}

}  // unnamed namespace
}  // namespace mongo
