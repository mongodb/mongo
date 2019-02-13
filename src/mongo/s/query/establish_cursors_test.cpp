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

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/json.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/s/catalog/type_shard.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/sharding_router_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

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

class EstablishCursorsTest : public ShardingTestFixture {
public:
    EstablishCursorsTest() : _nss("testdb.testcoll") {}

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
                stdx::make_unique<RemoteCommandTargeterMock>());
            targeter->setConnectionStringReturnValue(ConnectionString(kTestShardHosts[i]));
            targeter->setFindHostReturnValue(kTestShardHosts[i]);

            targeterFactory()->addTargeterToReturn(ConnectionString(kTestShardHosts[i]),
                                                   std::move(targeter));
        }

        setupShards(shards);
    }

protected:
    const NamespaceString _nss;
};

TEST_F(EstablishCursorsTest, NoRemotes) {
    std::vector<std::pair<ShardId, BSONObj>> remotes;
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
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

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

    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithNonretriableError) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

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
        return Status(ErrorCodes::FailedToParse, "failed to parse");
    });
    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithNonretriableErrorAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

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
        return Status(ErrorCodes::FailedToParse, "failed to parse");
    });
    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithRetriableErrorThenSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

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

    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, SingleRemoteRespondsWithRetriableErrorThenSuccessAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

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

    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, SingleRemoteMaxesOutRetriableErrors) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

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
    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, SingleRemoteMaxesOutRetriableErrorsAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{{kTestShardIds[0], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults

        // Failure to establish a cursor due to maxing out retriable errors on one remote (in this
        // case, the only remote) was ignored, since allowPartialResults is true, and one less
        // cursor was established.
        ASSERT_EQUALS(remotes.size() - 1, cursors.size());
    });

    // Remote repeatedly responds with retriable errors.
    for (int i = 0; i < kMaxRetries + 1; ++i) {
        onCommand([this](const RemoteCommandRequest& request) {
            ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
            return Status(ErrorCodes::HostUnreachable, "host unreachable");
        });
    }
    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, MultipleRemotesRespondWithSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
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

    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteRespondsWithNonretriableError) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
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
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    // Second remote responds with a non-retriable error.
    onCommand([this](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());
        return Status(ErrorCodes::FailedToParse, "failed to parse");
    });

    // Third remote responds with success (must give some response to mock network for each remote).
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest,
       MultipleRemotesOneRemoteRespondsWithNonretriableErrorAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
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
        return Status(ErrorCodes::FailedToParse, "failed to parse");
    });

    // Third remote responds with success (must give some response to mock network for each remote).
    onCommand([&](const RemoteCommandRequest& request) {
        ASSERT_EQ(_nss.coll(), request.cmdObj.firstElement().valueStringData());

        std::vector<BSONObj> batch = {fromjson("{_id: 1}"), fromjson("{_id: 2}")};
        CursorResponse cursorResponse(_nss, CursorId(123), batch);
        return cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteRespondsWithRetriableErrorThenSuccess) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
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

    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest,
       MultipleRemotesOneRemoteRespondsWithRetriableErrorThenSuccessAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
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

    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteMaxesOutRetriableErrors) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
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

    future.timed_get(kFutureTimeout);
}

TEST_F(EstablishCursorsTest, MultipleRemotesOneRemoteMaxesOutRetriableErrorsAllowPartialResults) {
    BSONObj cmdObj = fromjson("{find: 'testcoll'}");
    std::vector<std::pair<ShardId, BSONObj>> remotes{
        {kTestShardIds[0], cmdObj}, {kTestShardIds[1], cmdObj}, {kTestShardIds[2], cmdObj}};

    auto future = launchAsync([&] {
        auto cursors = establishCursors(operationContext(),
                                        executor(),
                                        _nss,
                                        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
                                        remotes,
                                        true);  // allowPartialResults
        // Failure to establish a cursor due to maxing out retriable errors on one remote was
        // ignored, since allowPartialResults is true, and one less cursor was established.
        ASSERT_EQUALS(remotes.size() - 1, cursors.size());
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

    future.timed_get(kFutureTimeout);
}

}  // namespace

}  // namespace mongo
