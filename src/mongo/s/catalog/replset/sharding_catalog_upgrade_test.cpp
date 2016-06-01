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

#include <string>
#include <vector>

#include "mongo/bson/json.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/rpc/metadata/repl_set_metadata.h"
#include "mongo/rpc/metadata/server_selection_metadata.h"
#include "mongo/s/catalog/config_server_version.h"
#include "mongo/s/catalog/replset/sharding_catalog_client_impl.h"
#include "mongo/s/catalog/replset/sharding_catalog_test_fixture.h"
#include "mongo/s/catalog/type_config_version.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/s/write_ops/batched_update_request.h"

namespace mongo {
namespace {

using executor::RemoteCommandRequest;
using executor::RemoteCommandResponse;
using std::string;
using std::vector;

const BSONObj kReplSecondaryOkMetadata{[] {
    BSONObjBuilder o;
    o.appendElements(rpc::ServerSelectionMetadata(true, boost::none).toBSON());
    o.append(rpc::kReplSetMetadataFieldName, 1);
    return o.obj();
}()};

TEST_F(ShardingCatalogTestFixture, UpgradeNotNeeded) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future =
        launchAsync([this] { ASSERT_OK(catalogClient()->initConfigVersion(operationContext())); });

    onFindCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        const auto& findCmd = request.cmdObj;
        ASSERT_EQ("version", findCmd["find"].str());
        checkReadConcern(findCmd, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

        BSONObj versionDoc(BSON("_id" << 1 << "minCompatibleVersion"
                                      << MIN_COMPATIBLE_CONFIG_VERSION
                                      << "currentVersion"
                                      << CURRENT_CONFIG_VERSION
                                      << "clusterId"
                                      << OID::gen()));

        return vector<BSONObj>{versionDoc};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogTestFixture, InitTargetError) {
    configTargeter()->setFindHostReturnValue({ErrorCodes::InternalError, "Bad test network"});

    auto future = launchAsync([this] {
        auto status = catalogClient()->initConfigVersion(operationContext());
        ASSERT_EQ(ErrorCodes::InternalError, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogTestFixture, InitIncompatibleVersion) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->initConfigVersion(operationContext());
        ASSERT_EQ(ErrorCodes::IncompatibleShardingConfigVersion, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) {
        BSONObj versionDoc(fromjson(R"({
                _id: 1,
                minCompatibleVersion: 2,
                currentVersion: 3,
                clusterId: ObjectId("55919cc6dbe86ce7ac056427")
            })"));

        return vector<BSONObj>{versionDoc};
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogTestFixture, InitClusterMultiVersion) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->initConfigVersion(operationContext());
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

TEST_F(ShardingCatalogTestFixture, InitInvalidConfigVersionDoc) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->initConfigVersion(operationContext());
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

TEST_F(ShardingCatalogTestFixture, InitNoVersionDocEmptyConfig) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future =
        launchAsync([this] { ASSERT_OK(catalogClient()->initConfigVersion(operationContext())); });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

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

        auto versionDocRes = VersionType::fromBSON(update->getUpdateExpr());
        ASSERT_OK(versionDocRes.getStatus());
        const VersionType& versionDoc = versionDocRes.getValue();

        ASSERT_EQ(MIN_COMPATIBLE_CONFIG_VERSION, versionDoc.getMinCompatibleVersion());
        ASSERT_EQ(CURRENT_CONFIG_VERSION, versionDoc.getCurrentVersion());
        ASSERT_TRUE(versionDoc.isClusterIdSet());
        ASSERT_FALSE(versionDoc.isExcludingMongoVersionsSet());
        ASSERT_FALSE(versionDoc.isUpgradeIdSet());
        ASSERT_FALSE(versionDoc.isUpgradeStateSet());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setN(1);
        response.setNModified(1);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogTestFixture, InitConfigWriteError) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->initConfigVersion(operationContext());
        ASSERT_EQ(ErrorCodes::ExceededTimeLimit, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        return fromjson(R"({
                ok: 1,
                nModified: 0,
                n: 0,
                writeErrors: [{
                    index: 0,
                    code: 50,
                    errmsg: "exceeded time limit"
                }]
            })");
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogTestFixture, InitVersionTooOld) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->initConfigVersion(operationContext());
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

TEST_F(ShardingCatalogTestFixture, InitVersionDuplicateKeyNoOpAfterRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future =
        launchAsync([this] { ASSERT_OK(catalogClient()->initConfigVersion(operationContext())); });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        ASSERT_EQ(string("update"), request.cmdObj.firstElementFieldName());

        return fromjson(R"({
                ok: 1,
                nModified: 0,
                n: 0,
                writeErrors: [{
                    index: 0,
                    code: 11000,
                    errmsg: "E11000 duplicate key error index: config.v.$_id_ dup key: { : 1.0 }"
                }]
            })");
    });

    // Retry starts here

    onFindCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        const auto& findCmd = request.cmdObj;
        ASSERT_EQ("version", findCmd["find"].str());
        checkReadConcern(findCmd, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

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

TEST_F(ShardingCatalogTestFixture, InitVersionDuplicateKeyNoConfigVersionAfterRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future =
        launchAsync([this] { ASSERT_OK(catalogClient()->initConfigVersion(operationContext())); });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        ASSERT_EQ(string("update"), request.cmdObj.firstElementFieldName());

        return fromjson(R"({
                ok: 1,
                nModified: 0,
                n: 0,
                writeErrors: [{
                    index: 0,
                    code: 11000,
                    errmsg: "E11000 duplicate key error index: config.v.$_id_ dup key: { : 1.0 }"
                }]
            })");
    });

    // Retry starts here

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

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

        auto versionDocRes = VersionType::fromBSON(update->getUpdateExpr());
        ASSERT_OK(versionDocRes.getStatus());
        const VersionType& versionDoc = versionDocRes.getValue();

        ASSERT_EQ(MIN_COMPATIBLE_CONFIG_VERSION, versionDoc.getMinCompatibleVersion());
        ASSERT_EQ(CURRENT_CONFIG_VERSION, versionDoc.getCurrentVersion());
        ASSERT_TRUE(versionDoc.isClusterIdSet());
        ASSERT_FALSE(versionDoc.isExcludingMongoVersionsSet());
        ASSERT_FALSE(versionDoc.isUpgradeIdSet());
        ASSERT_FALSE(versionDoc.isUpgradeStateSet());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setN(1);
        response.setNModified(1);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogTestFixture, InitVersionDuplicateKeyTooNewAfterRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->initConfigVersion(operationContext());
        ASSERT_EQ(ErrorCodes::IncompatibleShardingConfigVersion, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        ASSERT_EQ(string("update"), request.cmdObj.firstElementFieldName());

        return fromjson(R"({
                ok: 1,
                nModified: 0,
                n: 0,
                writeErrors: [{
                    index: 0,
                    code: 11000,
                    errmsg: "E11000 duplicate key error index: config.v.$_id_ dup key: { : 1.0 }"
                }]
            })");
    });

    // Retry starts here

    onFindCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        const auto& findCmd = request.cmdObj;
        ASSERT_EQ("version", findCmd["find"].str());
        checkReadConcern(findCmd, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

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

TEST_F(ShardingCatalogTestFixture, InitVersionDuplicateKeyMaxRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future = launchAsync([this] {
        auto status = catalogClient()->initConfigVersion(operationContext());
        ASSERT_EQ(ErrorCodes::IncompatibleShardingConfigVersion, status.code());
        ASSERT_FALSE(status.reason().empty());
    });

    const int maxRetry = 3;
    for (int x = 0; x < maxRetry; x++) {
        onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

        onCommand([](const RemoteCommandRequest& request) {
            ASSERT_EQ(HostAndPort("config:123"), request.target);
            ASSERT_EQ("config", request.dbname);

            ASSERT_EQ(string("update"), request.cmdObj.firstElementFieldName());

            return fromjson(R"({
                    ok: 1,
                    nModified: 0,
                    n: 0,
                    writeErrors: [{
                        index: 0,
                        code: 11000,
                        errmsg: "E11000 duplicate key error index: config.v.$_id_ dup key: { : 1 }"
                    }]
                })");
        });
    }

    future.timed_get(kFutureTimeout);
}

TEST_F(ShardingCatalogTestFixture, InitVersionUpsertNoMatchNoOpAfterRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future =
        launchAsync([this] { ASSERT_OK(catalogClient()->initConfigVersion(operationContext())); });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        ASSERT_EQ(string("update"), request.cmdObj.firstElementFieldName());

        return fromjson(R"({
                ok: 1,
                nModified: 0,
                n: 0
            })");
    });

    // Retry starts here

    onFindCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQUALS(kReplSecondaryOkMetadata, request.metadata);

        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        const auto& findCmd = request.cmdObj;
        ASSERT_EQ("version", findCmd["find"].str());
        checkReadConcern(findCmd, Timestamp(0, 0), repl::OpTime::kUninitializedTerm);

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

TEST_F(ShardingCatalogTestFixture, InitVersionUpsertNoMatchNoConfigVersionAfterRetry) {
    configTargeter()->setFindHostReturnValue(HostAndPort("config:123"));

    auto future =
        launchAsync([this] { ASSERT_OK(catalogClient()->initConfigVersion(operationContext())); });

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        ASSERT_EQ(string("update"), request.cmdObj.firstElementFieldName());

        return fromjson(R"({
                ok: 1,
                nModified: 0,
                n: 0
            })");
    });

    // Retry starts here

    onFindCommand([](const RemoteCommandRequest& request) { return vector<BSONObj>{}; });

    onCommand([](const RemoteCommandRequest& request) {
        ASSERT_EQ(HostAndPort("config:123"), request.target);
        ASSERT_EQ("config", request.dbname);

        ASSERT_EQUALS(BSON(rpc::kReplSetMetadataFieldName << 1), request.metadata);

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

        auto versionDocRes = VersionType::fromBSON(update->getUpdateExpr());
        ASSERT_OK(versionDocRes.getStatus());
        const VersionType& versionDoc = versionDocRes.getValue();

        ASSERT_EQ(MIN_COMPATIBLE_CONFIG_VERSION, versionDoc.getMinCompatibleVersion());
        ASSERT_EQ(CURRENT_CONFIG_VERSION, versionDoc.getCurrentVersion());
        ASSERT_TRUE(versionDoc.isClusterIdSet());
        ASSERT_FALSE(versionDoc.isExcludingMongoVersionsSet());
        ASSERT_FALSE(versionDoc.isUpgradeIdSet());
        ASSERT_FALSE(versionDoc.isUpgradeStateSet());

        BatchedCommandResponse response;
        response.setOk(true);
        response.setN(1);
        response.setNModified(1);

        return response.toBSON();
    });

    future.timed_get(kFutureTimeout);
}

}  // unnamed namespace
}  // namespace mongo
