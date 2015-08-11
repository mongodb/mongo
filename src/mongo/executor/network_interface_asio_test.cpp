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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/async_mock_stream_factory.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/rpc/command_reply_builder.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace executor {
namespace {

class NetworkInterfaceASIOTest : public mongo::unittest::Test {
public:
    void setUp() override {
        auto factory = stdx::make_unique<AsyncMockStreamFactory>();
        // keep unowned pointer, but pass ownership to NIA
        _streamFactory = factory.get();
        _net = stdx::make_unique<NetworkInterfaceASIO>(std::move(factory));
        _net->startup();
    }

    void tearDown() override {
        _net->shutdown();
    }

    NetworkInterface& net() {
        return *_net;
    }

    AsyncMockStreamFactory& streamFactory() {
        return *_streamFactory;
    }

private:
    AsyncMockStreamFactory* _streamFactory;
    std::unique_ptr<NetworkInterfaceASIO> _net;
};

TEST_F(NetworkInterfaceASIOTest, StartCommand) {
    TaskExecutor::CallbackHandle cb{};

    HostAndPort testHost{"localhost", 20000};

    stdx::promise<RemoteCommandResponse> prom{};

    bool callbackCalled = false;

    net().startCommand(cb,
                       RemoteCommandRequest(testHost, "testDB", BSON("foo" << 1), BSON("bar" << 1)),
                       [&](StatusWith<RemoteCommandResponse> resp) {
                           callbackCalled = true;

                           try {
                               prom.set_value(uassertStatusOK(resp));
                           } catch (...) {
                               prom.set_exception(std::current_exception());
                           }
                       });

    auto fut = prom.get_future();

    auto stream = streamFactory().blockUntilStreamExists(testHost);

    // Allow stream to connect.
    {
        stream->waitUntilBlocked();
        auto guard = MakeGuard([&] { stream->unblock(); });
    }

    log() << "connected";

    uint32_t isMasterMsgId = 0;

    {
        stream->waitUntilBlocked();
        auto guard = MakeGuard([&] { stream->unblock(); });

        log() << "NIA blocked after writing isMaster request";

        // Check that an isMaster has been run on the stream
        std::vector<uint8_t> messageData = stream->popWrite();
        Message msg(messageData.data(), false);

        auto request = rpc::makeRequest(&msg);

        ASSERT_EQ(request->getCommandName(), "isMaster");
        ASSERT_EQ(request->getDatabase(), "admin");

        isMasterMsgId = msg.header().getId();

        // Check that we used OP_QUERY.
        ASSERT(request->getProtocol() == rpc::Protocol::kOpQuery);
    }

    rpc::LegacyReplyBuilder replyBuilder;
    replyBuilder.setMetadata(BSONObj());
    replyBuilder.setCommandReply(BSON("minWireVersion" << mongo::minWireVersion << "maxWireVersion"
                                                       << mongo::maxWireVersion));
    auto replyMsg = replyBuilder.done();
    replyMsg->header().setResponseTo(isMasterMsgId);

    {
        stream->waitUntilBlocked();
        auto guard = MakeGuard([&] { stream->unblock(); });

        log() << "NIA blocked before reading isMaster reply header";

        // write out the full message now, even though another read() call will read the rest.
        auto hdrBytes = reinterpret_cast<const uint8_t*>(replyMsg->header().view2ptr());

        stream->pushRead({hdrBytes, hdrBytes + sizeof(MSGHEADER::Value)});

        auto dataBytes = reinterpret_cast<const uint8_t*>(replyMsg->buf());
        auto pastHeader = dataBytes;
        std::advance(pastHeader, sizeof(MSGHEADER::Value));  // skip the header this time

        stream->pushRead({pastHeader, dataBytes + static_cast<std::size_t>(replyMsg->size())});
    }

    {
        stream->waitUntilBlocked();
        auto guard = MakeGuard([&] { stream->unblock(); });
        log() << "NIA blocked before reading isMaster reply data";
    }

    auto expectedCommandReply = BSON("boop"
                                     << "bop"
                                     << "ok" << 1.0);
    auto expectedMetadata = BSON("meep"
                                 << "beep");

    {
        stream->waitUntilBlocked();
        auto guard = MakeGuard([&] { stream->unblock(); });

        log() << "blocked after write(), reading user command request";

        std::vector<uint8_t> messageData{stream->popWrite()};

        Message msg(messageData.data(), false);
        auto request = rpc::makeRequest(&msg);

        // the command we requested should be running.
        ASSERT_EQ(request->getCommandName(), "foo");
        ASSERT_EQ(request->getDatabase(), "testDB");

        // we should be using op command given our previous isMaster reply.
        ASSERT(request->getProtocol() == rpc::Protocol::kOpCommandV1);

        rpc::CommandReplyBuilder replyBuilder;
        replyBuilder.setMetadata(expectedMetadata).setCommandReply(expectedCommandReply);
        auto replyMsg = replyBuilder.done();

        replyMsg->header().setResponseTo(msg.header().getId());

        // write out the full message now, even though another read() call will read the rest.
        auto hdrBytes = reinterpret_cast<const uint8_t*>(replyMsg->header().view2ptr());

        stream->pushRead({hdrBytes, hdrBytes + sizeof(MSGHEADER::Value)});

        auto dataBytes = reinterpret_cast<const uint8_t*>(replyMsg->buf());
        auto pastHeader = dataBytes;
        std::advance(pastHeader, sizeof(MSGHEADER::Value));  // skip the header this time

        stream->pushRead({pastHeader, dataBytes + static_cast<std::size_t>(replyMsg->size())});
    }

    {
        stream->waitUntilBlocked();
        auto guard = MakeGuard([&] { stream->unblock(); });
    }

    {
        stream->waitUntilBlocked();
        auto guard = MakeGuard([&] { stream->unblock(); });
    }

    auto res = fut.get();

    ASSERT(callbackCalled);
    ASSERT_EQ(res.data, expectedCommandReply);
    ASSERT_EQ(res.metadata, expectedMetadata);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
