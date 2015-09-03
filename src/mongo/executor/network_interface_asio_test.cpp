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

#include <boost/optional.hpp>

#include "mongo/base/status_with.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/async_mock_stream_factory.h"
#include "mongo/executor/async_timer_mock.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/executor/test_network_connection_hook.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace executor {
namespace {

HostAndPort testHost{"localhost", 20000};

// Utility function to use with mock streams
RemoteCommandResponse simulateIsMaster(RemoteCommandRequest request) {
    ASSERT_EQ(std::string{request.cmdObj.firstElementFieldName()}, "isMaster");
    ASSERT_EQ(request.dbname, "admin");

    RemoteCommandResponse response;
    response.data = BSON("minWireVersion" << mongo::minWireVersion << "maxWireVersion"
                                          << mongo::maxWireVersion);
    return response;
}

class NetworkInterfaceASIOTest : public mongo::unittest::Test {
public:
    void setUp() override {
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

    NetworkInterfaceASIO& net() {
        return *_net;
    }

    AsyncMockStreamFactory& streamFactory() {
        return *_streamFactory;
    }

    AsyncTimerFactoryMock& timerFactory() {
        return *_timerFactory;
    }

protected:
    AsyncTimerFactoryMock* _timerFactory;
    AsyncMockStreamFactory* _streamFactory;
    std::unique_ptr<NetworkInterfaceASIO> _net;
};

// A mock class mimicking TaskExecutor::CallbackState, does nothing.
class MockCallbackState : public TaskExecutor::CallbackState {
public:
    MockCallbackState() = default;
    void cancel() override {}
    void waitForCompletion() override {}
};

TEST_F(NetworkInterfaceASIOTest, CancelMissingOperation) {
    // This is just a sanity check, this action should have no effect.
    auto cbState = std::make_shared<MockCallbackState>();
    TaskExecutor::CallbackHandle cb(cbState);
    net().cancelCommand(cb);
}

TEST_F(NetworkInterfaceASIOTest, CancelOperation) {
    auto cbState = std::make_shared<MockCallbackState>();
    TaskExecutor::CallbackHandle cbh(cbState);

    stdx::promise<bool> canceled;

    // Kick off our operation
    net().startCommand(cbh,
                       RemoteCommandRequest(testHost, "testDB", BSON("a" << 1), BSONObj()),
                       [&canceled](StatusWith<RemoteCommandResponse> response) {
                           canceled.set_value(response == ErrorCodes::CallbackCanceled);
                       });

    // Create and initialize a stream so operation can begin
    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request)
                               -> RemoteCommandResponse { return simulateIsMaster(request); });

    {
        // Cancel operation while blocked in the write for determinism. By calling cancel here we
        // ensure that it is not a no-op and that the asio::operation_aborted error will always
        // be returned to the NIA.
        WriteEvent write{stream};
        net().cancelCommand(cbh);
    }

    // Wait for op to complete, assert that it was canceled.
    auto canceledFuture = canceled.get_future();
    ASSERT(canceledFuture.get());
}

TEST_F(NetworkInterfaceASIOTest, AsyncOpTimeout) {
    stdx::promise<bool> timedOut;
    auto timedOutFuture = timedOut.get_future();

    // Kick off operation
    TaskExecutor::CallbackHandle cb{};
    Milliseconds timeout(1000);
    net().startCommand(cb,
                       {testHost, "testDB", BSON("a" << 1), BSONObj(), timeout},
                       [&timedOut](StatusWith<RemoteCommandResponse> response) {
                           timedOut.set_value(response == ErrorCodes::ExceededTimeLimit);
                       });

    // Create and initialize a stream so operation can begin
    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // Simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request)
                               -> RemoteCommandResponse { return simulateIsMaster(request); });

    {
        // Wait for the operation to block on write so we know it's been added.
        WriteEvent write{stream};

        // Get the timer factory
        auto& factory = timerFactory();

        // Advance clock but not enough to force a timeout, assert still active
        factory.fastForward(Milliseconds(500));
        ASSERT(timedOutFuture.wait_for(Milliseconds(1)) == stdx::future_status::timeout);

        // Advance clock and force timeout
        factory.fastForward(Milliseconds(800));
    }
    ASSERT(timedOutFuture.get());
}

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
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request)
                               -> RemoteCommandResponse { return simulateIsMaster(request); });

    auto expectedMetadata = BSON("meep"
                                 << "beep");
    auto expectedCommandReply = BSON("boop"
                                     << "bop"
                                     << "ok" << 1.0);

    // simulate user command
    stream->simulateServer(rpc::Protocol::kOpCommandV1,
                           [&](RemoteCommandRequest request) -> RemoteCommandResponse {
                               ASSERT_EQ(std::string{request.cmdObj.firstElementFieldName()},
                                         "foo");
                               ASSERT_EQ(request.dbname, "testDB");

                               RemoteCommandResponse response;
                               response.data = expectedCommandReply;
                               response.metadata = expectedMetadata;
                               return response;
                           });

    auto res = fut.get();

    ASSERT(callbackCalled);
    ASSERT_EQ(res.data, expectedCommandReply);
    ASSERT_EQ(res.metadata, expectedMetadata);
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

    stdx::promise<void> done;
    bool statusCorrect = false;
    auto doneFuture = done.get_future();

    net().startCommand({},
                       {testHost,
                        "blah",
                        BSON("foo"
                             << "bar")},
                       [&](StatusWith<RemoteCommandResponse> result) {
                           statusCorrect = (result == validationFailedStatus);
                           done.set_value();
                       });

    auto stream = streamFactory().blockUntilStreamExists(testHost);

    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request) -> RemoteCommandResponse {
                               RemoteCommandResponse response;
                               response.data = BSON("minWireVersion"
                                                    << mongo::minWireVersion << "maxWireVersion"
                                                    << mongo::maxWireVersion << "TESTKEY"
                                                    << "TESTVALUE");
                               return response;
                           });

    // we should stop here.
    doneFuture.get();
    ASSERT(statusCorrect);
    ASSERT(validateCalled);
    ASSERT(hostCorrect);
    ASSERT(isMasterReplyCorrect);

    ASSERT(!makeRequestCalled);
    ASSERT(!handleReplyCalled);
}

TEST_F(NetworkInterfaceASIOConnectionHookTest, MakeRequestReturnsError) {
    bool makeRequestCalled = false;
    bool handleReplyCalled = false;

    Status makeRequestError{ErrorCodes::DBPathInUse, "bloooh"};

    start(makeTestHook(
        [&](const HostAndPort& remoteHost, const RemoteCommandResponse& isMasterReply)
            -> Status { return Status::OK(); },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            makeRequestCalled = true;
            return makeRequestError;
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) {
            handleReplyCalled = true;
            return Status::OK();
        }));

    stdx::promise<void> done;
    bool statusCorrect = false;
    auto doneFuture = done.get_future();

    net().startCommand({},
                       {testHost,
                        "blah",
                        BSON("foo"
                             << "bar")},
                       [&](StatusWith<RemoteCommandResponse> result) {
                           statusCorrect = (result == makeRequestError);
                           done.set_value();
                       });

    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request)
                               -> RemoteCommandResponse { return simulateIsMaster(request); });

    // We should stop here.
    doneFuture.get();
    ASSERT(statusCorrect);
    ASSERT(makeRequestCalled);
    ASSERT(!handleReplyCalled);
}

TEST_F(NetworkInterfaceASIOConnectionHookTest, MakeRequestReturnsNone) {
    bool makeRequestCalled = false;
    bool handleReplyCalled = false;

    start(makeTestHook(
        [&](const HostAndPort& remoteHost, const RemoteCommandResponse& isMasterReply)
            -> Status { return Status::OK(); },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            makeRequestCalled = true;
            return {boost::none};
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) {
            handleReplyCalled = true;
            return Status::OK();
        }));

    stdx::promise<void> done;
    auto doneFuture = done.get_future();
    bool statusCorrect = false;

    auto commandRequest = BSON("foo"
                               << "bar");

    auto commandReply = BSON("foo"
                             << "boo"
                             << "ok" << 1.0);

    auto metadata = BSON("aaa"
                         << "bbb");

    net().startCommand({},
                       {testHost, "blah", commandRequest},
                       [&](StatusWith<RemoteCommandResponse> result) {
                           statusCorrect =
                               (result.isOK() && (result.getValue().data == commandReply) &&
                                (result.getValue().metadata == metadata));
                           done.set_value();
                       });


    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request)
                               -> RemoteCommandResponse { return simulateIsMaster(request); });

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
    doneFuture.get();
    ASSERT(statusCorrect);
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
                                    << "ok" << 1.0);
    BSONObj hookReplyMetadata = BSON("1111" << 2222);

    Status handleReplyError{ErrorCodes::AuthSchemaIncompatible, "daowdjkpowkdjpow"};

    start(makeTestHook(
        [&](const HostAndPort& remoteHost, const RemoteCommandResponse& isMasterReply)
            -> Status { return Status::OK(); },
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

    stdx::promise<void> done;
    auto doneFuture = done.get_future();
    bool statusCorrect = false;
    auto commandRequest = BSON("foo"
                               << "bar");
    net().startCommand({},
                       {testHost, "blah", commandRequest},
                       [&](StatusWith<RemoteCommandResponse> result) {
                           statusCorrect = (result == handleReplyError);
                           done.set_value();
                       });

    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request)
                               -> RemoteCommandResponse { return simulateIsMaster(request); });

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

    doneFuture.get();
    ASSERT(statusCorrect);
    ASSERT(makeRequestCalled);
    ASSERT(handleReplyCalled);
    ASSERT(handleReplyArgumentCorrect);
}

TEST_F(NetworkInterfaceASIOTest, setAlarm) {
    stdx::promise<bool> nearFuture;
    stdx::future<bool> executed = nearFuture.get_future();

    // set a first alarm, to execute after "expiration"
    Date_t expiration = net().now() + Milliseconds(100);
    net().setAlarm(
        expiration,
        [this, expiration, &nearFuture]() { nearFuture.set_value(net().now() >= expiration); });

    // wait enough time for first alarm to execute
    auto status = executed.wait_for(Milliseconds(5000));

    // assert that not only did it execute, but executed after "expiration"
    ASSERT(status == stdx::future_status::ready);
    ASSERT(executed.get());

    // set an alarm for the future, kill interface, ensure it didn't execute
    stdx::promise<bool> farFuture;
    stdx::future<bool> executed2 = farFuture.get_future();

    expiration = net().now() + Milliseconds(99999999);
    net().setAlarm(expiration, [this, &farFuture]() { farFuture.set_value(true); });

    net().shutdown();

    status = executed2.wait_for(Milliseconds(0));
    ASSERT(status == stdx::future_status::timeout);
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

    std::promise<void> done;

    net().startCommand({},
                       {testHost, "blah", BSON("ping" << 1)},
                       [&](StatusWith<RemoteCommandResponse> result) { done.set_value(); });

    auto stream = streamFactory().blockUntilStreamExists(testHost);
    ConnectEvent{stream}.skip();

    // simulate isMaster reply.
    stream->simulateServer(rpc::Protocol::kOpQuery,
                           [](RemoteCommandRequest request)
                               -> RemoteCommandResponse { return simulateIsMaster(request); });

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
    done.get_future().get();
    ASSERT(wroteRequestMetadata);
    ASSERT(gotReplyMetadata);
}

}  // namespace
}  // namespace executor
}  // namespace mongo
