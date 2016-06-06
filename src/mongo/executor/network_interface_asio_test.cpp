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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/async_mock_stream_factory.h"
#include "mongo/executor/async_timer_mock.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/executor/network_interface_asio_test_utils.h"
#include "mongo/executor/test_network_connection_hook.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace executor {
namespace {

HostAndPort testHost{"localhost", 20000};

void initWireSpecMongoD() {
    WireSpec& spec = WireSpec::instance();
    // accept from any version
    spec.minWireVersionIncoming = RELEASE_2_4_AND_BEFORE;
    spec.maxWireVersionIncoming = COMMANDS_ACCEPT_WRITE_CONCERN;
    // connect to any version
    spec.minWireVersionOutgoing = RELEASE_2_4_AND_BEFORE;
    spec.maxWireVersionOutgoing = COMMANDS_ACCEPT_WRITE_CONCERN;
}

// Utility function to use with mock streams
RemoteCommandResponse simulateIsMaster(RemoteCommandRequest request) {
    ASSERT_EQ(std::string{request.cmdObj.firstElementFieldName()}, "isMaster");
    ASSERT_EQ(request.dbname, "admin");

    RemoteCommandResponse response;
    response.data = BSON("minWireVersion" << mongo::WireSpec::instance().minWireVersionIncoming
                                          << "maxWireVersion"
                                          << mongo::WireSpec::instance().maxWireVersionIncoming);
    return response;
}

class NetworkInterfaceASIOTest : public mongo::unittest::Test {
public:
    void setUp() override {
        initWireSpecMongoD();
        NetworkInterfaceASIO::Options options;

        // Use mock timer factory
        auto timerFactory = stdx::make_unique<AsyncTimerFactoryMock>();
        _timerFactory = timerFactory.get();
        options.timerFactory = std::move(timerFactory);

        auto factory = stdx::make_unique<AsyncMockStreamFactory>();
        // keep unowned pointer, but pass ownership to NIA
        _streamFactory = factory.get();
        options.streamFactory = std::move(factory);
        _net = stdx::make_unique<NetworkInterfaceASIO>(std::move(options));
        _net->startup();
    }

    void tearDown() override {
        if (!_net->inShutdown()) {
            _net->shutdown();
        }
    }

    Deferred<StatusWith<RemoteCommandResponse>> startCommand(
        const TaskExecutor::CallbackHandle& cbHandle, const RemoteCommandRequest& request) {
        Deferred<StatusWith<RemoteCommandResponse>> deferredResponse;
        ASSERT_OK(net().startCommand(
            cbHandle,
            request,
            [deferredResponse](StatusWith<RemoteCommandResponse> response) mutable {
                deferredResponse.emplace(std::move(response));
            }));
        return deferredResponse;
    }

    // Helper to run startCommand and wait for it
    StatusWith<RemoteCommandResponse> startCommandSync(const RemoteCommandRequest& request) {
        auto deferred = startCommand(makeCallbackHandle(), request);

        // wait for the operation to complete
        auto& result = deferred.get();
        return result;
    }

    NetworkInterfaceASIO& net() {
        return *_net;
    }

    AsyncMockStreamFactory& streamFactory() {
        return *_streamFactory;
    }

    AsyncTimerFactoryMock& timerFactory() {
        return *_timerFactory;
    }

    void assertNumOps(uint64_t canceled, uint64_t timedOut, uint64_t failed, uint64_t succeeded) {
        ASSERT(net().getNumCanceledOps() == canceled);
        ASSERT(net().getNumTimedOutOps() == timedOut);
        ASSERT(net().getNumFailedOps() == failed);
        ASSERT(net().getNumSucceededOps() == succeeded);
    }

protected:
    AsyncTimerFactoryMock* _timerFactory;
    AsyncMockStreamFactory* _streamFactory;
    std::unique_ptr<NetworkInterfaceASIO> _net;
};

TEST_F(NetworkInterfaceASIOTest, CancelMissingOperation) {
    // This is just a sanity check, this action should have no effect.
    net().cancelCommand(makeCallbackHandle());
    assertNumOps(0u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceASIOTest, CancelOperation) {
    auto cbh = makeCallbackHandle();

    // Kick off our operation
    auto deferred =
        startCommand(cbh, RemoteCommandRequest(testHost, "testDB", BSON("a" << 1), BSONObj()));

    // Create and initialize a stream so operation can begin
    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    {
        // Cancel operation while blocked in the write for determinism. By calling cancel here we
        // ensure that it is not a no-op and that the asio::operation_aborted error will always
        // be returned to the NIA.
        WriteEvent write{stream};
        net().cancelCommand(cbh);
    }

    // Wait for op to complete, assert that it was canceled.
    auto& result = deferred.get();
    ASSERT(result == ErrorCodes::CallbackCanceled);
    assertNumOps(1u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceASIOTest, ImmediateCancel) {
    auto cbh = makeCallbackHandle();

    auto deferred =
        startCommand(cbh, RemoteCommandRequest(testHost, "testDB", BSON("a" << 1), BSONObj()));

    // Cancel immediately
    net().cancelCommand(cbh);

    // Allow stream to connect so operation can return
    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    auto& result = deferred.get();
    ASSERT(result == ErrorCodes::CallbackCanceled);
    // expect 0 completed ops because the op was canceled before getting a connection
    assertNumOps(1u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceASIOTest, LateCancel) {
    auto cbh = makeCallbackHandle();
    auto deferred =
        startCommand(cbh, RemoteCommandRequest(testHost, "testDB", BSON("a" << 1), BSONObj()));

    // Allow stream to connect so operation can return
    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    // Simulate user command
    stream->simulateServer(rpc::Protocol::kOpCommandV1,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               RemoteCommandResponse response;
                               response.data = BSONObj();
                               response.metadata = BSONObj();
                               return response;
                           });

    // Allow to complete, then cancel, nothing should happen.
    deferred.get();
    net().cancelCommand(cbh);

    assertNumOps(0u, 0u, 0u, 1u);
}

TEST_F(NetworkInterfaceASIOTest, CancelWithNetworkError) {
    auto cbh = makeCallbackHandle();
    auto deferred =
        startCommand(cbh, RemoteCommandRequest(testHost, "testDB", BSON("a" << 1), BSONObj()));

    // Create and initialize a stream so operation can begin
    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    {
        WriteEvent{stream}.skip();
        ReadEvent read{stream};

        // Trigger both a cancellation and a network error
        stream->setError(make_error_code(ErrorCodes::HostUnreachable));
        net().cancelCommand(cbh);
    }

    // Wait for op to complete, assert that cancellation error had precedence.
    auto& result = deferred.get();
    ASSERT(result == ErrorCodes::CallbackCanceled);
    assertNumOps(1u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceASIOTest, CancelWithTimeout) {
    auto cbh = makeCallbackHandle();
    auto deferred =
        startCommand(cbh, RemoteCommandRequest(testHost, "testDB", BSON("a" << 1), BSONObj()));

    // Create and initialize a stream so operation can begin
    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    {
        WriteEvent write{stream};

        // Trigger both a cancellation and a timeout
        net().cancelCommand(cbh);
        timerFactory().fastForward(Milliseconds(500));
    }

    // Wait for op to complete, assert that cancellation error had precedence.
    auto& result = deferred.get();
    ASSERT(result == ErrorCodes::CallbackCanceled);
    assertNumOps(1u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceASIOTest, TimeoutWithNetworkError) {
    auto cbh = makeCallbackHandle();
    auto deferred = startCommand(
        cbh,
        RemoteCommandRequest(testHost, "testDB", BSON("a" << 1), BSONObj(), Milliseconds(1000)));

    // Create and initialize a stream so operation can begin
    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    {
        WriteEvent{stream}.skip();
        ReadEvent read{stream};

        // Trigger both a timeout and a network error
        stream->setError(make_error_code(ErrorCodes::HostUnreachable));
        timerFactory().fastForward(Milliseconds(2000));
    }

    // Wait for op to complete, assert that timeout had precedence.
    auto& result = deferred.get();
    ASSERT(result == ErrorCodes::ExceededTimeLimit);
    assertNumOps(0u, 1u, 1u, 0u);
}

TEST_F(NetworkInterfaceASIOTest, CancelWithTimeoutAndNetworkError) {
    auto cbh = makeCallbackHandle();
    auto deferred = startCommand(
        cbh,
        RemoteCommandRequest(testHost, "testDB", BSON("a" << 1), BSONObj(), Milliseconds(1000)));

    // Create and initialize a stream so operation can begin
    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    {
        WriteEvent{stream}.skip();
        ReadEvent read{stream};

        // Trigger a timeout, a cancellation, and a network error
        stream->setError(make_error_code(ErrorCodes::HostUnreachable));
        timerFactory().fastForward(Milliseconds(2000));
        net().cancelCommand(cbh);
    }

    // Wait for op to complete, assert that the cancellation had precedence.
    auto& result = deferred.get();
    ASSERT(result == ErrorCodes::CallbackCanceled);
    assertNumOps(1u, 0u, 0u, 0u);
}

TEST_F(NetworkInterfaceASIOTest, AsyncOpTimeout) {
    // Kick off operation
    auto cb = makeCallbackHandle();
    Milliseconds timeout(1000);
    auto deferred = startCommand(cb, {testHost, "testDB", BSON("a" << 1), BSONObj(), timeout});

    // Create and initialize a stream so operation can begin
    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // Simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    {
        // Wait for the operation to block on write so we know it's been added.
        WriteEvent write{stream};

        // Get the timer factory
        auto& factory = timerFactory();

        // Advance clock but not enough to force a timeout, assert still active
        factory.fastForward(Milliseconds(500));
        ASSERT(!deferred.hasCompleted());

        // Advance clock and force timeout
        factory.fastForward(Milliseconds(800));
    }

    auto& result = deferred.get();
    ASSERT(result == ErrorCodes::ExceededTimeLimit);
    assertNumOps(0u, 1u, 1u, 0u);
}

TEST_F(NetworkInterfaceASIOTest, StartCommand) {
    auto deferred =
        startCommand(makeCallbackHandle(),
                     RemoteCommandRequest(testHost, "testDB", BSON("foo" << 1), BSON("bar" << 1)));

    auto stream = streamFactory().blockUntilStreamExists(testHost);

    // Allow stream to connect.
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    auto expectedMetadata = BSON("meep"
                                 << "beep");
    auto expectedCommandReply = BSON("boop"
                                     << "bop"
                                     << "ok"
                                     << 1.0);

    // simulate user command
    stream->simulateServer(
        rpc::Protocol::kOpCommandV1, [&](RemoteCommandRequest request) -> RemoteCommandResponse {
            ASSERT_EQ(std::string{request.cmdObj.firstElementFieldName()}, "foo");
            ASSERT_EQ(request.dbname, "testDB");

            RemoteCommandResponse response;
            response.data = expectedCommandReply;
            response.metadata = expectedMetadata;
            return response;
        });

    auto& res = deferred.get();

    auto response = uassertStatusOK(res);
    ASSERT_EQ(response.data, expectedCommandReply);
    ASSERT_EQ(response.metadata, expectedMetadata);
    assertNumOps(0u, 0u, 0u, 1u);
}

TEST_F(NetworkInterfaceASIOTest, InShutdown) {
    ASSERT_FALSE(net().inShutdown());
    net().shutdown();
    ASSERT(net().inShutdown());
}

TEST_F(NetworkInterfaceASIOTest, StartCommandReturnsNotOKIfShutdownHasStarted) {
    net().shutdown();
    ASSERT_NOT_OK(net().startCommand(makeCallbackHandle(),
                                     RemoteCommandRequest{},
                                     [&](StatusWith<RemoteCommandResponse> resp) {}));
}

class MalformedMessageTest : public NetworkInterfaceASIOTest {
public:
    using MessageHook = stdx::function<void(MsgData::View)>;

    void runMessageTest(ErrorCodes::Error code, bool loadBody, MessageHook hook) {
        // Kick off our operation
        auto deferred =
            startCommand(makeCallbackHandle(),
                         RemoteCommandRequest(testHost, "testDB", BSON("ping" << 1), BSONObj()));

        // Wait for it to block waiting for a write
        auto stream = streamFactory().blockUntilStreamExists(testHost);
        ConnectEvent{stream}.skip();
        stream->simulateServer(rpc::Protocol::kOpQuery,
                               [](RemoteCommandRequest request) -> RemoteCommandResponse {
                                   return simulateIsMaster(request);
                               });

        uint32_t messageId = 0;

        {
            // Get the appropriate message id
            WriteEvent write{stream};
            std::vector<uint8_t> messageData = stream->popWrite();
            messageId =
                MsgData::ConstView(reinterpret_cast<const char*>(messageData.data())).getId();
        }

        // Build a mock reply message
        auto replyBuilder = rpc::makeReplyBuilder(rpc::Protocol::kOpCommandV1);
        replyBuilder->setCommandReply(BSON("hello!" << 1));
        replyBuilder->setMetadata(BSONObj());

        auto message = replyBuilder->done();
        message.header().setResponseToMsgId(messageId);

        auto actualSize = message.header().getLen();

        // Allow caller to mess with the Message
        hook(message.header());

        {
            // Load the header
            ReadEvent read{stream};
            auto headerBytes = reinterpret_cast<const uint8_t*>(message.header().view2ptr());
            stream->pushRead({headerBytes, headerBytes + sizeof(MSGHEADER::Value)});
        }

        if (loadBody) {
            // Load the body if we need to
            ReadEvent read{stream};
            auto dataBytes = reinterpret_cast<const uint8_t*>(message.buf());
            auto body = dataBytes;
            std::advance(body, sizeof(MSGHEADER::Value));
            stream->pushRead({body, dataBytes + static_cast<std::size_t>(actualSize)});
        }

        auto& response = deferred.get();
        ASSERT(response == code);
        assertNumOps(0u, 0u, 1u, 0u);
    }
};

TEST_F(MalformedMessageTest, messageHeaderWrongResponseTo) {
    runMessageTest(ErrorCodes::ProtocolError, false, [](MsgData::View message) {
        message.setResponseToMsgId(message.getResponseToMsgId() + 1);
    });
}

TEST_F(MalformedMessageTest, messageHeaderlenZero) {
    runMessageTest(
        ErrorCodes::InvalidLength, false, [](MsgData::View message) { message.setLen(0); });
}

TEST_F(MalformedMessageTest, MessageHeaderLenTooSmall) {
    runMessageTest(ErrorCodes::InvalidLength, false, [](MsgData::View message) {
        message.setLen(6);
    });  // min is 16
}

TEST_F(MalformedMessageTest, MessageHeaderLenTooLarge) {
    runMessageTest(ErrorCodes::InvalidLength, false, [](MsgData::View message) {
        message.setLen(48000001);
    });  // max is 48000000
}

TEST_F(MalformedMessageTest, MessageHeaderLenNegative) {
    runMessageTest(
        ErrorCodes::InvalidLength, false, [](MsgData::View message) { message.setLen(-1); });
}

TEST_F(MalformedMessageTest, MessageLenSmallerThanActual) {
    runMessageTest(ErrorCodes::InvalidBSON, true, [](MsgData::View message) {
        message.setLen(message.getLen() - 10);
    });
}

TEST_F(MalformedMessageTest, FailedToReadAllBytesForMessage) {
    runMessageTest(ErrorCodes::InvalidLength, true, [](MsgData::View message) {
        message.setLen(message.getLen() + 100);
    });
}

TEST_F(MalformedMessageTest, UnsupportedOpcode) {
    runMessageTest(ErrorCodes::UnsupportedFormat, true, [](MsgData::View message) {
        message.setOperation(2222);
    });
}

TEST_F(MalformedMessageTest, MismatchedOpcode) {
    runMessageTest(ErrorCodes::UnsupportedFormat, true, [](MsgData::View message) {
        message.setOperation(2006);
    });
}

class NetworkInterfaceASIOConnectionHookTest : public NetworkInterfaceASIOTest {
public:
    void setUp() override {}

    void start(std::unique_ptr<NetworkConnectionHook> hook) {
        auto factory = stdx::make_unique<AsyncMockStreamFactory>();
        // keep unowned pointer, but pass ownership to NIA
        _streamFactory = factory.get();
        NetworkInterfaceASIO::Options options{};
        options.streamFactory = std::move(factory);
        options.networkConnectionHook = std::move(hook);
        _net = stdx::make_unique<NetworkInterfaceASIO>(std::move(options));
        _net->startup();
    }
};

TEST_F(NetworkInterfaceASIOConnectionHookTest, InvalidIsMaster) {
    auto validationFailedStatus =
        Status(ErrorCodes::InterruptedDueToReplStateChange, "operation was interrupted");

    start(makeTestHook(
        [&](const HostAndPort& remoteHost, const RemoteCommandResponse& isMasterReply) {
            return Status(ErrorCodes::UnknownError, "unused");
        },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            return {boost::none};
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) {
            return Status::OK();
        }));

    auto deferred = startCommand(makeCallbackHandle(),
                                 {testHost,
                                  "blah",
                                  BSON("foo"
                                       << "bar")});

    auto stream = streamFactory().blockUntilStreamExists(testHost);

    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               RemoteCommandResponse response;
                               response.data = BSON("ok" << 0.0 << "errmsg"
                                                         << "operation was interrupted"
                                                         << "code"
                                                         << 11602);
                               return response;
                           });

    // we should stop here.
    auto& res = deferred.get();

    ASSERT(res == validationFailedStatus);

    assertNumOps(0u, 0u, 1u, 0u);
}

TEST_F(NetworkInterfaceASIOConnectionHookTest, ValidateHostInvalid) {
    bool validateCalled = false;
    bool hostCorrect = false;
    bool isMasterReplyCorrect = false;
    bool makeRequestCalled = false;
    bool handleReplyCalled = false;

    auto validationFailedStatus = Status(ErrorCodes::AlreadyInitialized, "blahhhhh");

    start(makeTestHook(
        [&](const HostAndPort& remoteHost, const RemoteCommandResponse& isMasterReply) {
            validateCalled = true;
            hostCorrect = (remoteHost == testHost);
            isMasterReplyCorrect = (isMasterReply.data["TESTKEY"].str() == "TESTVALUE");
            return validationFailedStatus;
        },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            makeRequestCalled = true;
            return {boost::none};
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) {
            handleReplyCalled = true;
            return Status::OK();
        }));

    auto deferred = startCommand(makeCallbackHandle(),
                                 {testHost,
                                  "blah",
                                  BSON("foo"
                                       << "bar")});

    auto stream = streamFactory().blockUntilStreamExists(testHost);

    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(
        rpc::Protocol::kOpQuery, [](RemoteCommandRequest request) -> RemoteCommandResponse {
            RemoteCommandResponse response;
            response.data =
                BSON("minWireVersion" << mongo::WireSpec::instance().minWireVersionIncoming
                                      << "maxWireVersion"
                                      << mongo::WireSpec::instance().maxWireVersionIncoming
                                      << "TESTKEY"
                                      << "TESTVALUE");
            return response;
        });

    // we should stop here.
    auto& res = deferred.get();

    // auto result = uassertStatusOK(res);
    ASSERT(res == validationFailedStatus);
    ASSERT(validateCalled);
    ASSERT(hostCorrect);
    ASSERT(isMasterReplyCorrect);

    ASSERT(!makeRequestCalled);
    ASSERT(!handleReplyCalled);
    assertNumOps(0u, 0u, 1u, 0u);
}

TEST_F(NetworkInterfaceASIOConnectionHookTest, MakeRequestReturnsError) {
    bool makeRequestCalled = false;
    bool handleReplyCalled = false;

    Status makeRequestError{ErrorCodes::DBPathInUse, "bloooh"};

    start(makeTestHook(
        [&](const HostAndPort& remoteHost, const RemoteCommandResponse& isMasterReply) -> Status {
            return Status::OK();
        },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            makeRequestCalled = true;
            return makeRequestError;
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) {
            handleReplyCalled = true;
            return Status::OK();
        }));

    auto deferred = startCommand(makeCallbackHandle(),
                                 {testHost,
                                  "blah",
                                  BSON("foo"
                                       << "bar")});

    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    // We should stop here.
    auto& res = deferred.get();

    ASSERT(res == makeRequestError);
    ASSERT(makeRequestCalled);
    ASSERT(!handleReplyCalled);
    assertNumOps(0u, 0u, 1u, 0u);
}

TEST_F(NetworkInterfaceASIOConnectionHookTest, MakeRequestReturnsNone) {
    bool makeRequestCalled = false;
    bool handleReplyCalled = false;

    start(makeTestHook(
        [&](const HostAndPort& remoteHost, const RemoteCommandResponse& isMasterReply) -> Status {
            return Status::OK();
        },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            makeRequestCalled = true;
            return {boost::none};
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) {
            handleReplyCalled = true;
            return Status::OK();
        }));

    auto commandRequest = BSON("foo"
                               << "bar");

    auto commandReply = BSON("foo"
                             << "boo"
                             << "ok"
                             << 1.0);

    auto metadata = BSON("aaa"
                         << "bbb");

    auto deferred = startCommand(makeCallbackHandle(), {testHost, "blah", commandRequest});

    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    // Simulate user command.
    stream->simulateServer(rpc::Protocol::kOpCommandV1,
                           [&](RemoteCommandRequest request) -> RemoteCommandResponse {
                               ASSERT_EQ(commandRequest, request.cmdObj);

                               RemoteCommandResponse response;
                               response.data = commandReply;
                               response.metadata = metadata;
                               return response;
                           });

    // We should get back the reply now.
    auto& result = deferred.get();

    ASSERT(result.isOK());
    ASSERT(result.getValue().data == commandReply);
    ASSERT(result.getValue().metadata == metadata);
    assertNumOps(0u, 0u, 0u, 1u);
}

TEST_F(NetworkInterfaceASIOConnectionHookTest, HandleReplyReturnsError) {
    bool makeRequestCalled = false;

    bool handleReplyCalled = false;
    bool handleReplyArgumentCorrect = false;

    BSONObj hookCommandRequest = BSON("1ddd"
                                      << "fff");
    BSONObj hookRequestMetadata = BSON("wdwd" << 1212);

    BSONObj hookCommandReply = BSON("blah"
                                    << "blah"
                                    << "ok"
                                    << 1.0);
    BSONObj hookReplyMetadata = BSON("1111" << 2222);

    Status handleReplyError{ErrorCodes::AuthSchemaIncompatible, "daowdjkpowkdjpow"};

    start(makeTestHook(
        [&](const HostAndPort& remoteHost, const RemoteCommandResponse& isMasterReply) -> Status {
            return Status::OK();
        },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            makeRequestCalled = true;
            return {boost::make_optional<RemoteCommandRequest>(
                {testHost, "foo", hookCommandRequest, hookRequestMetadata})};

        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) {
            handleReplyCalled = true;
            handleReplyArgumentCorrect =
                (response.data == hookCommandReply) && (response.metadata == hookReplyMetadata);
            return handleReplyError;
        }));

    auto commandRequest = BSON("foo"
                               << "bar");
    auto deferred = startCommand(makeCallbackHandle(), {testHost, "blah", commandRequest});

    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    // Simulate hook reply
    stream->simulateServer(rpc::Protocol::kOpCommandV1,
                           [&](RemoteCommandRequest request) -> RemoteCommandResponse {
                               ASSERT_EQ(request.cmdObj, hookCommandRequest);
                               ASSERT_EQ(request.metadata, hookRequestMetadata);

                               RemoteCommandResponse response;
                               response.data = hookCommandReply;
                               response.metadata = hookReplyMetadata;
                               return response;
                           });

    auto& result = deferred.get();

    ASSERT(result == handleReplyError);
    ASSERT(makeRequestCalled);
    ASSERT(handleReplyCalled);
    ASSERT(handleReplyArgumentCorrect);
    assertNumOps(0u, 0u, 1u, 0u);
}

TEST_F(NetworkInterfaceASIOTest, SetAlarm) {
    // set a first alarm, to execute after "expiration"
    Date_t expiration = net().now() + Milliseconds(100);

    Deferred<Date_t> deferred;
    ASSERT_OK(net().setAlarm(
        expiration, [this, expiration, deferred]() mutable { deferred.emplace(net().now()); }));

    // Get our timer factory
    auto& factory = timerFactory();

    // force the alarm to fire
    factory.fastForward(Milliseconds(5000));

    // assert that it executed after "expiration"
    auto& result = deferred.get();
    ASSERT(result >= expiration);

    expiration = net().now() + Milliseconds(99999999);
    Deferred<bool> deferred2;
    ASSERT_OK(net().setAlarm(expiration, [this, deferred2]() mutable { deferred2.emplace(true); }));

    net().shutdown();
    ASSERT(!deferred2.hasCompleted());
}

TEST_F(NetworkInterfaceASIOTest, SetAlarmReturnsNotOKIfShutdownHasStarted) {
    net().shutdown();
    ASSERT_NOT_OK(net().setAlarm(net().now() + Milliseconds(100), [] {}));
}

class NetworkInterfaceASIOMetadataTest : public NetworkInterfaceASIOTest {
protected:
    void setUp() override {}

    void start(std::unique_ptr<rpc::EgressMetadataHook> metadataHook) {
        auto factory = stdx::make_unique<AsyncMockStreamFactory>();
        _streamFactory = factory.get();
        NetworkInterfaceASIO::Options options{};
        options.streamFactory = std::move(factory);
        options.metadataHook = std::move(metadataHook);
        _net = stdx::make_unique<NetworkInterfaceASIO>(std::move(options));
        _net->startup();
    }
};

class TestMetadataHook : public rpc::EgressMetadataHook {
public:
    TestMetadataHook(bool* wroteRequestMetadata, bool* gotReplyMetadata)
        : _wroteRequestMetadata(wroteRequestMetadata), _gotReplyMetadata(gotReplyMetadata) {}

    Status writeRequestMetadata(const HostAndPort& requestDestination,
                                BSONObjBuilder* metadataBob) override {
        metadataBob->append("foo", "bar");
        *_wroteRequestMetadata = true;
        return Status::OK();
    }

    Status readReplyMetadata(const HostAndPort& replySource, const BSONObj& metadataObj) override {
        *_gotReplyMetadata = (metadataObj["baz"].str() == "garply");
        return Status::OK();
    }

private:
    bool* _wroteRequestMetadata;
    bool* _gotReplyMetadata;
};

TEST_F(NetworkInterfaceASIOMetadataTest, Metadata) {
    bool wroteRequestMetadata = false;
    bool gotReplyMetadata = false;
    start(stdx::make_unique<TestMetadataHook>(&wroteRequestMetadata, &gotReplyMetadata));

    auto deferred = startCommand(makeCallbackHandle(), {testHost, "blah", BSON("ping" << 1)});

    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               return simulateIsMaster(request);
                           });

    // Simulate hook reply
    stream->simulateServer(rpc::Protocol::kOpCommandV1,
                           [&](RemoteCommandRequest request) -> RemoteCommandResponse {
                               ASSERT(request.metadata["foo"].str() == "bar");
                               RemoteCommandResponse response;
                               response.data = BSON("ok" << 1);
                               response.metadata = BSON("baz"
                                                        << "garply");
                               return response;
                           });

    deferred.get();
    ASSERT(wroteRequestMetadata);
    ASSERT(gotReplyMetadata);
    assertNumOps(0u, 0u, 0u, 1u);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
