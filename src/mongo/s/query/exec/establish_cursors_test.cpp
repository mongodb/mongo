/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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
#include <fmt/format.h>
// IWYU pragma: no_include "cxxabi.h"
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/global_catalog/type_shard.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielders.h"
#include "mongo/db/query/client_cursor/cursor_id.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/sharding_environment/sharding_mongos_test_fixture.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/s/query/exec/establish_cursors.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/uuid.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>

namespace mongo {

namespace {

using executor::RemoteCommandRequest;

const int kMaxRetries = 3;
const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};

std::vector<UUID> extractOperationKeys(BSONObj obj) {
    ASSERT_TRUE(obj.hasField("operationKeys")) << obj;

    std::vector<UUID> opKeys;
    for (auto&& elem : obj["operationKeys"].Array()) {
        auto opKey = unittest::assertGet(UUID::parse(elem));
        opKeys.push_back(std::move(opKey));
    }
    return opKeys;
}

class EstablishCursorsTest : public ShardingTestFixture {
public:
    EstablishCursorsTest()
        : _nss(NamespaceString::createNamespaceString_forTest("testdb.testcoll")) {}

    void setUp() override {
        ShardingTestFixture::setUp();

        configTargeter()->setFindHostReturnValue(kTestConfigShardHost);

        std::vector<ShardType> shards;

        for (size_t i = 0; i < kTestShardIds.size(); i++) {
            ShardType shardType;
            shardType.setName(kTestShardIds[i].toString());
            shardType.setHost(kTestShardHosts[i].toString());

            shards.push_back(shardType);

            std::unique_ptr<RemoteCommandTargeterMock> targeter(
                std::make_unique<RemoteCommandTargeterMock>());
            _targeters.push_back(targeter.get());

            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHosts[i]));
            targeter->setFindHostReturnValue(kTestShardHosts[i]);

            targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHosts[i]),
                                                   std::move(targeter));
        }

        setupShards(shards);
    }

    /**
     * Mock a response for a killOperations command.
     */
    void expectKillOperations(size_t expected, std::vector<UUID> expectedOpKeys = {}) {
        for (size_t i = 0; i < expected; i++) {
            onCommand([&](const RemoteCommandRequest& request) {
                ASSERT_EQ(DatabaseName::kAdmin, request.dbname) << request;
                ASSERT_TRUE(request.cmdObj.hasField("_killOperations")) << request;

                ASSERT_TRUE(request.cmdObj.hasField("operationKeys")) << request;
                if (expectedOpKeys.size()) {
                    auto sentOpKeys = extractOperationKeys(request.cmdObj);
                    ASSERT_EQ(expectedOpKeys.size(), sentOpKeys.size());
                    std::sort(expectedOpKeys.begin(), expectedOpKeys.end());
                    std::sort(sentOpKeys.begin(), sentOpKeys.end());
                    for (size_t i = 0; i < expectedOpKeys.size(); i++) {
                        ASSERT_EQ(expectedOpKeys[i], sentOpKeys[i]);
                    }
                }

                return BSON("ok" << 1);
            });
        }
    }

    void onCommandThrowStaleConifg(ShardId shardId) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

            BSONObjBuilder builder;
            StaleConfigInfo staleConfigExtraInfo(
                NamespaceString::createNamespaceString_forTest("test.testcoll"),
                {},  // shardVersion
                boost::none,
                shardId);
            staleConfigExtraInfo.serialize(&builder);

            return createErrorCursorResponse(
                Status(ErrorCodes::StaleConfig, "fake stale config", builder.obj()));
        });
    }

    ChunkManager createChunkManager(const UUID& uuid, const NamespaceString& nss) {
        ShardKeyPattern sk{fromjson("{x: 1, _id: 1}")};
        std::deque<DocumentSource::GetNextResult> configData{
            Document(fromjson("{_id: {x: {$minKey: 1}, _id: {$minKey: 1}}, max: {x: 0.0, _id: "
                              "0.0}, shard: 'shard1'}")),
            Document(fromjson("{_id: {x: 0.0, _id: 0.0}, max: {x: {$maxKey: 1}, _id: {$maxKey: "
                              "1}}, shard: 'shard2' }"))};
        const OID epoch = OID::gen();
        std::vector<ChunkType> chunks;
        for (const auto& chunkData : configData) {
            const auto bson = chunkData.getDocument().toBson();
            ChunkRange range{bson.getField("_id").Obj().getOwned(),
                             bson.getField("max").Obj().getOwned()};
            ShardId shard{std::string{bson.getField("shard").valueStringDataSafe()}};
            chunks.emplace_back(uuid,
                                std::move(range),
                                ChunkVersion({epoch, Timestamp(1, 1)}, {1, 0}),
                                std::move(shard));
        }

        auto rt = RoutingTableHistory::makeNew(nss,
                                               uuid,
                                               sk.getKeyPattern(),
                                               false, /* unsplittable */
                                               nullptr,
                                               false,
                                               epoch,
                                               Timestamp(1, 1),
                                               boost::none /* timeseriesFields */,
                                               boost::none /* reshardingFields */,
                                               false,
                                               chunks);

        return ChunkManager(
            ShardingTestFixtureCommon::makeStandaloneRoutingTableHistory(std::move(rt)),
            boost::none);
    }

protected:
    const NamespaceString _nss;
    std::vector<RemoteCommandTargeterMock*> _targeters;  // Targeters are owned by the factory.
};

TEST_F(EstablishCursorsTest, NoRemotes) {
    std::vector<AsyncRequestsSender::Request> remotes;
    auto cursors = establishCursors(operationContext(),
                                    executor(),
                                    _nss,
                                    ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                    remotes,
                                    false);  // allowPartialResults


    ASSERT_EQUALS(remotes.size(), cursors.size());
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        false);  // allowPartialResults
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });

    // Remote responds.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithInvalidMessage) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj}};

    AsyncRequestsSender::ShardHostMap designatedHosts;
    auto shard0Secondary = HostAndPort("SecondaryHostShard0", 12345);
    _targeters[0]->setConnectionStringReturnValue(
        ConnectionString::forReplicaSet("shard0_rs"_sd, {kTestShardHosts[0], shard0Secondary}));
    designatedHosts[kTestShardIds[0]] = shard0Secondary;

    // Intentionally throw an exception during validation.
    FailPointEnableBlock failPoint("throwDuringCursorResponseValidation");

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false,  // allowPartialResults
                                       nullptr /* RoutingContext */,
                                       Shard::RetryPolicy::kIdempotent,
                                       {},  // providedOpKeys
                                       designatedHosts),
                      ExceptionFor<ErrorCodes::FailedToParse>);
    });

    // Remote responds.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();

    // This ensures the fail point has been hit exactly once.
    failPoint->waitForTimesEntered(failPoint.initialTimesEntered() + 1);
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithSuccessWithRoutingContext) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj}};

    auto uuid = UUID::gen();
    stdx::unordered_map<NamespaceString, CollectionRoutingInfo> criMap = {
        {_nss,
         CollectionRoutingInfo(
             createChunkManager(uuid, _nss),
             DatabaseTypeValueHandle(DatabaseType{
                 _nss.dbName(), kTestShardIds[0], DatabaseVersion(uuid, Timestamp{1, 1})}))}};
    auto routingCtx = RoutingContext::createSynthetic(criMap);

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        false /* allowPartialResults */,
                                        routingCtx.get());
        ASSERT_EQUALS(remotes.size(), cursors.size());
        routingCtx->validateOnContextEnd();
    });

    // Remote responds.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithDesignatedHost) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj}};

    AsyncRequestsSender::ShardHostMap designatedHosts;
    auto shard0Secondary = HostAndPort("SecondaryHostShard0", 12345);
    _targeters[0]->setConnectionStringReturnValue(
        ConnectionString::forReplicaSet("shard0_rs"_sd, {kTestShardHosts[0], shard0Secondary}));
    designatedHosts[kTestShardIds[0]] = shard0Secondary;
    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        false,  // allowPartialResults
                                        nullptr /* RoutingContext */,
                                        Shard::RetryPolicy::kIdempotent,
                                        {},  // providedOpKeys
                                        designatedHosts);
        ASSERT_EQUALS(remotes.size(), cursors.size());
        ASSERT_EQUALS(cursors[0].getHostAndPort(), shard0Secondary);
    });

    // Remote responds.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithNonretriableError) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::FailedToParse>);
    });

    // Remote responds with non-retriable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return createErrorCursorResponse(Status(ErrorCodes::FailedToParse, "failed to parse"));
    });
    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteInterruptedWhileCommandInFlight) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj},
    };

    auto barrier = std::make_shared<unittest::Barrier>(2);
    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::CursorKilled>);
        barrier->countDownAndWait();
    });

    auto seenOpKey = UUID::gen();
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        ASSERT_TRUE(request.cmdObj.hasField("clientOperationKey")) << request;
        seenOpKey = unittest::assertGet(UUID::parse(request.cmdObj["clientOperationKey"]));

        // Now that our "remote" has received the request, interrupt the opCtx which the cursor is
        // running under.
        {
            ClientLock lk(operationContext()->getClient());
            operationContext()->getServiceContext()->killOperation(
                lk, operationContext(), ErrorCodes::CursorKilled);
        }

        // Wait for the kill to take since there is a race between response and kill.
        barrier->countDownAndWait();

        CursorResponse cursorResponse(_nss, CursorId(123), {});
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // We were interrupted so establishCursors is forced to send a killOperations out of paranoia.
    expectKillOperations(1, {seenOpKey});

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithNonretriableErrorAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        // A non-retriable error is not ignored even though allowPartialResults is true.
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       true),  // allowPartialResults
                      ExceptionFor<ErrorCodes::FailedToParse>);
    });

    // Remote responds with non-retriable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return createErrorCursorResponse(Status(ErrorCodes::FailedToParse, "failed to parse"));
    });
    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithRetriableErrorThenSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        false);  // allowPartialResults
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });

    // Remote responds with retriable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // Remote responds with success.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithRetriableErrorThenSuccessAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });

    // Remote responds with retriable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // We still retry up to the max retries, even if allowPartialResults is true.
    // Remote responds with success.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteMaxesOutRetriableErrors) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::HostUnreachable>);
    });

    // Remote repeatedly responds with retriable errors.
    for (int i = 0; i < kMaxRetries + 1; ++i) {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
            return Status(ErrorCodes::HostUnreachable, "host unreachable");
        });
    }

    // Expect a killOperations for the remote which was not reachable.
    expectKillOperations(1);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, SingleRemoteMaxesOutRetriableErrorsAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults

        // Failure to establish a cursor due to maxing out retriable errors on one remote (in this
        // case, the only remote) was ignored, since allowPartialResults is true. The cursor entry
        // is marked as 'partialResultReturned:true', with a CursorId of 0 and no HostAndPort.
        ASSERT_EQ(cursors.size(), 1);
        ASSERT(cursors.front().getHostAndPort().empty());
        ASSERT_EQ(cursors.front().getShardId(), kTestShardIds[0]);
        ASSERT(cursors.front().getCursorResponse().getPartialResultsReturned());
        ASSERT_EQ(cursors.front().getCursorResponse().getCursorId(), CursorId{0});
    });

    // Remote repeatedly responds with retriable errors.
    for (int i = 0; i < kMaxRetries + 1; ++i) {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
            return Status(ErrorCodes::HostUnreachable, "host unreachable");
        });
    }

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesRespondWithSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        false);  // allowPartialResults
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });

    // All remotes respond with success.
    for (auto it = remotes.begin(); it != remotes.end(); ++it) {
        onCommand([&](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

            std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
            CursorResponse cursorResponse(_nss, CursorId(123), batch);
            return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteRespondsWithNonretriableError) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::FailedToParse>);
    });

    // First remote responds with success.
    auto seenOpKey = UUID::gen();
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        ASSERT_TRUE(request.cmdObj.hasField("clientOperationKey")) << request;
        seenOpKey = unittest::assertGet(UUID::parse(request.cmdObj["clientOperationKey"]));

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a non-retriable error.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        // All commands receive the same opKey.
        ASSERT_TRUE(request.cmdObj.hasField("clientOperationKey")) << request;
        auto opKey = unittest::assertGet(UUID::parse(request.cmdObj["clientOperationKey"]));
        ASSERT_EQ(seenOpKey, opKey);

        return createErrorCursorResponse(Status(ErrorCodes::FailedToParse, "failed to parse"));
    });

    // Third remote responds with success (must give some response to mock network for each remote).
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        // All commands receive the same opKey.
        ASSERT_TRUE(request.cmdObj.hasField("clientOperationKey")) << request;
        auto opKey = unittest::assertGet(UUID::parse(request.cmdObj["clientOperationKey"]));
        ASSERT_EQ(seenOpKey, opKey);

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Expect two killOperation commands, one for each remote which responded with a cursor.
    expectKillOperations(2, {seenOpKey});

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, AcceptsCustomOpKeys) {
    std::vector<UUID> providedOpKeys = {UUID::gen(), UUID::gen()};
    auto cmdObj0 = BSON("find" << "testcoll"
                               << "clientOperationKey" << providedOpKeys[0]);
    auto cmdObj1 = BSON("find" << "testcoll"
                               << "clientOperationKey" << providedOpKeys[1]);
    auto cmdObj2 = BSON("find" << "testcoll"
                               << "clientOperationKey" << providedOpKeys[1]);
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj0}, {kTestShardIds[1], cmdObj1}, {kTestShardIds[2], cmdObj2}};

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false,  // allowPartialResults
                                       nullptr /* RoutingContext */,
                                       Shard::RetryPolicy::kIdempotent,
                                       providedOpKeys),
                      ExceptionFor<ErrorCodes::FailedToParse>);
    });

    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        // All commands use the opKey they were given.
        ASSERT_TRUE(request.cmdObj.hasField("clientOperationKey")) << request;
        auto opKey = unittest::assertGet(UUID::parse(request.cmdObj["clientOperationKey"]));
        ASSERT_TRUE(std::find(providedOpKeys.begin(), providedOpKeys.end(), opKey) !=
                    providedOpKeys.end());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a non-retriable error.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        // All commands use the opKey they were given.
        ASSERT_TRUE(request.cmdObj.hasField("clientOperationKey")) << request;
        auto opKey = unittest::assertGet(UUID::parse(request.cmdObj["clientOperationKey"]));
        ASSERT_TRUE(std::find(providedOpKeys.begin(), providedOpKeys.end(), opKey) !=
                    providedOpKeys.end());

        return createErrorCursorResponse(Status(ErrorCodes::FailedToParse, "failed to parse"));
    });

    // Third remote responds with success (must give some response to mock network for each remote).
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        // All commands use the opKey they were given.
        ASSERT_TRUE(request.cmdObj.hasField("clientOperationKey")) << request;
        auto opKey = unittest::assertGet(UUID::parse(request.cmdObj["clientOperationKey"]));
        ASSERT_TRUE(std::find(providedOpKeys.begin(), providedOpKeys.end(), opKey) !=
                    providedOpKeys.end());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Expect two killOperation commands, one for each remote which responded with a cursor. Both
    // should include all provided opKeys.
    expectKillOperations(2, providedOpKeys);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest,
       MultipleRemotesOneRemoteRespondsWithNonretriableErrorAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        // Failure is reported even though allowPartialResults was true.
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       true),  // allowPartialResults
                      ExceptionFor<ErrorCodes::FailedToParse>);
    });

    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a non-retriable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return createErrorCursorResponse(Status(ErrorCodes::FailedToParse, "failed to parse"));
    });

    // Third remote responds with success (must give some response to mock network for each remote).
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Expect two killOperation commands, one for each remote which responded with a cursor.
    expectKillOperations(2);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteRespondsWithRetriableErrorThenSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        false);  // allowPartialResults
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });

    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a retriable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // Third remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with success on retry.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest,
       MultipleRemotesOneRemoteRespondsWithRetriableErrorThenSuccessAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults
        // We still retry up to the max retries, even if allowPartialResults is true.
        ASSERT_EQUALS(remotes.size(), cursors.size());
    });
    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a retriable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // Third remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with success on retry.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteMaxesOutRetriableErrors) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::HostUnreachable>);
    });

    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a retriable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // Third remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote maxes out remaining retries.
    for (int i = 0; i < kMaxRetries; ++i) {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
            return Status(ErrorCodes::HostUnreachable, "host unreachable");
        });
    }

    // Expect two killOperation commands, one for each remote which responded with a cursor.
    expectKillOperations(2);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteMaxesOutRetriableErrorsAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults
        // Failure to establish a cursor due to maxing out retriable errors on one remote was
        // ignored, since allowPartialResults is true. The cursor entry for that shard is marked
        // 'partialResultReturned:true', with a CursorId of 0 and no HostAndPort.
        ASSERT_EQ(remotes.size(), cursors.size());
        for (auto&& cursor : cursors) {
            const bool isMaxedOutShard = (cursor.getShardId() == kTestShardIds[1]);
            ASSERT_EQ(cursor.getHostAndPort().empty(), isMaxedOutShard);
            ASSERT_EQ(cursor.getCursorResponse().getPartialResultsReturned(), isMaxedOutShard);
            ASSERT(isMaxedOutShard ? cursor.getCursorResponse().getCursorId() == CursorId{0}
                                   : cursor.getCursorResponse().getCursorId() > CursorId{0});
        }
    });

    // First remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a retriable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::HostUnreachable, "host unreachable");
    });

    // Third remote responds with success.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote maxes out remaining retries.
    for (int i = 0; i < kMaxRetries; ++i) {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
            return Status(ErrorCodes::HostUnreachable, "host unreachable");
        });
    }

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesAllMaxOutRetriableErrorsAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    // Failure to establish a cursor due to maxing out retriable errors on all three remotes
    // returns an error, despite allowPartialResults being true.
    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults
        // allowPartialResults is true so ignore the fact that all remotes will haved failed
        // to establish a cursor due to maxing out retriable errors. The cursor entry
        // is marked as 'partialResultReturned:true', with a CursorId of 0 and no HostAndPort.
        ASSERT_EQ(remotes.size(), cursors.size());
        for (auto&& cursor : cursors) {
            ASSERT(cursor.getHostAndPort().empty());
            ASSERT(cursor.getCursorResponse().getPartialResultsReturned());
            ASSERT_EQ(cursor.getCursorResponse().getCursorId(), CursorId{0});
        }
    });

    // All remotes always respond with retriable errors.
    for (auto it = remotes.begin(); it != remotes.end(); ++it) {
        for (int i = 0; i < kMaxRetries + 1; ++i) {
            onCommand([&](const RemoteCommandRequest& request) {
                ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
                return Status(ErrorCodes::HostUnreachable, "host unreachable");
            });
        }
    }

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, InterruptedWithDanglingRemoteRequest) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj},
        {kTestShardIds[1], cmdObj},
    };
    unittest::Barrier barrier(2);

    // Hang in ARS::next when there is exactly 1 remote that hasn't replied yet.
    // This failpoint is important to ensure establishCursors' check for _interruptStatus.isOK()
    // happens after this unittest does opCtx->killOperation().
    auto fpNext = globalFailPointRegistry().find("hangBeforePollResponse");
    invariant(fpNext);
    auto timesHitNext = fpNext->setMode(FailPoint::alwaysOn, 0, BSON("remotesLeft" << 1));

    auto future = launchAsync([&] {
        ScopeGuard guard([&] { barrier.countDownAndWait(); });
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::CursorKilled>);
    });

    // First remote responds.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        CursorResponse cursorResponse(_nss, CursorId(123), {});
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Wait until we hit the hangBeforePollResponse failpoint with remotesLeft 1.
    // This ensures the first response has been processed.
    fpNext->waitForTimesEntered(timesHitNext + 1);
    // Now allow the thread calling ARS::next to continue.
    fpNext->setMode(FailPoint::off);

    // Now we're processing the request for the second remote. Kill the opCtx
    // instead of responding.
    onCommand([&](auto&&) {
        ClientLock lk(operationContext()->getClient());
        operationContext()->getServiceContext()->killOperation(
            lk, operationContext(), ErrorCodes::CursorKilled);
        CursorResponse cursorResponse(_nss, CursorId(123), {});
        // Wait until the kill takes.
        barrier.countDownAndWait();
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Now ARS::next should check that the opCtx has been marked killed, and return a
    // failing response to establishCursors, which should clean up by sending kill commands.
    expectKillOperations(2);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, InterruptedAfterErrorResponse) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj},
        {kTestShardIds[1], cmdObj},
    };
    unittest::Barrier barrier(2);

    // Hang in ARS::next when there is exactly 1 remote that hasn't replied yet.
    // This failpoint is important to ensure establishCursors' check for _interruptStatus.isOK()
    // happens after this unittest does opCtx->killOperation().
    auto fpNext = globalFailPointRegistry().find("hangBeforePollResponse");
    invariant(fpNext);
    auto timesHitNext = fpNext->setMode(FailPoint::alwaysOn, 0, BSON("remotesLeft" << 1));

    auto future = launchAsync([&] {
        ScopeGuard guard([&] { barrier.countDownAndWait(); });
        // Make sure we get CursorKilled and not StaleConfig.
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::CursorKilled>);
    });

    // First remote responds StaleConfig.
    onCommandThrowStaleConifg(kTestShardIds[0]);

    // Wait until we hit the hangBeforePollResponse failpoint with remotesLeft 1.
    // This ensures the first response has been processed.
    fpNext->waitForTimesEntered(timesHitNext + 1);
    // Now allow the thread calling ARS::next to continue.
    fpNext->setMode(FailPoint::off);

    // Now we're processing the request for the second remote. Kill the opCtx
    // instead of responding.
    onCommand([&](auto&&) {
        ClientLock lk(operationContext()->getClient());
        operationContext()->getServiceContext()->killOperation(
            lk, operationContext(), ErrorCodes::CursorKilled);
        CursorResponse cursorResponse(_nss, CursorId(123), {});
        // Wait until the kill takes.
        barrier.countDownAndWait();
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // We didn't get a cursor back from the first response so we should only kill for the second
    // response.
    expectKillOperations(1);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, FailedUnyieldAfterErrorResponse) {
    struct MockResourceYielder : public ResourceYielder {
        void yield(OperationContext*) override {}
        void unyield(OperationContext*) override {
            if (_count++) {
                uasserted(ErrorCodes::LockTimeout, "Simulated error");
            }
        }
        int _count = 0;
    };
    struct MockResourceYielderFactory : public ResourceYielderFactory {
        std::unique_ptr<ResourceYielder> make(OperationContext* opCtx,
                                              StringData cmdName) const override {
            return std::make_unique<MockResourceYielder>();
        }
    };
    ResourceYielderFactory::set(*getService(), std::make_unique<MockResourceYielderFactory>());

    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{
        {kTestShardIds[0], cmdObj},
        {kTestShardIds[1], cmdObj},
    };

    // Hang in ARS::next when there is exactly 1 remote that hasn't replied yet.
    // This failpoint is important to ensure establishCursors' check for _interruptStatus.isOK()
    // happens after this unittest does opCtx->killOperation().
    auto fpNext = globalFailPointRegistry().find("hangBeforePollResponse");
    invariant(fpNext);
    auto timesHitNext = fpNext->setMode(FailPoint::alwaysOn, 0, BSON("remotesLeft" << 1));

    auto future = launchAsync([&] {
        // Make sure we get LockTimeout and not StaleConfig.
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::LockTimeout>);
    });

    // First remote responds StaleConfig.
    onCommandThrowStaleConifg(kTestShardIds[0]);

    // Wait until we hit the hangBeforePollResponse failpoint with remotesLeft 1.
    // This ensures the first response has been processed.
    fpNext->waitForTimesEntered(timesHitNext + 1);
    // Now allow the thread calling ARS::next to continue.
    fpNext->setMode(FailPoint::off);

    onCommand([&](auto&&) {
        // MSARS will get a second response so it won't get interrupted while waiting, but will
        // subsequently fail to unyield, so this cursor will get thrown away. CursorEstablisher
        // should still send a kill command.
        CursorResponse cursorResponse(_nss, CursorId(123), {});
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // We didn't get a cursor back from the first response so we should only kill for the second
    // response.
    expectKillOperations(1);

    future.default_timed_get();
}

TEST_F(EstablishCursorsTest, MultipleRemotesMultipleDifferentErrors) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<AsyncRequestsSender::Request> remotes{{kTestShardIds[0], cmdObj},
                                                      {kTestShardIds[1], cmdObj}};

    // Prefer throwing LockTimeout.
    auto future = launchAsync([&] {
        ASSERT_THROWS(establishCursors(operationContext(),
                                       executor(),
                                       _nss,
                                       ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                       remotes,
                                       false),  // allowPartialResults
                      ExceptionFor<ErrorCodes::LockTimeout>);
    });

    // First remote responds with stale config error.
    onCommandThrowStaleConifg(kTestShardIds[0]);

    // Second remote responds encounters simulated yield error.
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return createErrorCursorResponse(
            Status(ErrorCodes::LockTimeout, "fake unable to acquire ticket2"));
    });

    future.default_timed_get();
}

}  // namespace

}  // namespace mongo
