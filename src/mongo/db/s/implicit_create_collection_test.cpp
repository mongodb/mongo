/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/implicit_create_collection.h"
#include "mongo/s/request_types/create_collection_gen.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

class ImplicitCreateTest : public ShardServerTestFixture {
public:
    void expectConfigCreate(const NamespaceString& expectedNss, const Status& response) {
        onCommand([&](const executor::RemoteCommandRequest& request) {
            auto configHostStatus = configTargeterMock()->findHost(nullptr, {});
            ASSERT_OK(configHostStatus.getStatus());
            auto configHost = configHostStatus.getValue();

            ASSERT_EQ(configHost, request.target);
            auto cmdName = request.cmdObj.firstElement().fieldName();
            ASSERT_EQ(ConfigsvrCreateCollection::kCommandName, cmdName);

            ASSERT_EQ("admin", request.dbname);
            ASSERT_EQ(expectedNss.ns(), request.cmdObj.firstElement().String());

            BSONObjBuilder responseBuilder;
            CommandHelpers::appendCommandStatusNoThrow(responseBuilder, response);
            return responseBuilder.obj();
        });
    }
};

TEST_F(ImplicitCreateTest, NormalCreate) {
    const NamespaceString kNs("test.user");
    auto future = launchAsync([this, &kNs] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ASSERT_OK(onCannotImplicitlyCreateCollection(opCtx.get(), kNs));
    });

    expectConfigCreate(kNs, Status::OK());

    future.timed_get(kFutureTimeout);
}

TEST_F(ImplicitCreateTest, CanCallOnCannotImplicitAgainAfterError) {
    const NamespaceString kNs("test.user");
    auto future = launchAsync([this, &kNs] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        auto status = onCannotImplicitlyCreateCollection(opCtx.get(), kNs);
        ASSERT_EQ(ErrorCodes::FailPointEnabled, status);
    });

    // return a non retryable error (just for testing) so the handler won't retry.
    expectConfigCreate(kNs, {ErrorCodes::FailPointEnabled, "deliberate error"});

    future.timed_get(kFutureTimeout);


    // Retry, but this time config server will return success

    future = launchAsync([this, &kNs] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ASSERT_OK(onCannotImplicitlyCreateCollection(opCtx.get(), kNs));
    });

    expectConfigCreate(kNs, Status::OK());

    future.timed_get(kFutureTimeout);
}

TEST_F(ImplicitCreateTest, ShouldNotCallConfigCreateIfCollectionExists) {
    const NamespaceString kNs("test.user");
    auto future = launchAsync([this, &kNs] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        auto status = onCannotImplicitlyCreateCollection(opCtx.get(), kNs);
        ASSERT_EQ(ErrorCodes::FailPointEnabled, status);
    });

    // return a non retryable error (just for testing) so the handler won't retry.
    expectConfigCreate(kNs, {ErrorCodes::FailPointEnabled, "deliberate error"});

    future.timed_get(kFutureTimeout);

    // Simulate config server successfully creating the collection despite returning error.
    DBDirectClient client(operationContext());
    BSONObj result;
    ASSERT_TRUE(
        client.runCommand(kNs.db().toString(), BSON("create" << kNs.coll().toString()), result));

    // Retry, but this time config server will return success

    future = launchAsync([this, &kNs] {
        ON_BLOCK_EXIT([&] { Client::destroy(); });
        Client::initThreadIfNotAlready("Test");
        auto opCtx = cc().makeOperationContext();
        ASSERT_OK(onCannotImplicitlyCreateCollection(opCtx.get(), kNs));
    });

    // Not expecting this shard to send any remote command.

    future.timed_get(kFutureTimeout);
}

}  // unnamed namespace
}  // namespace mongo
