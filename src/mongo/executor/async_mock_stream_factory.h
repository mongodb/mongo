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

#pragma once

#include <asio.hpp>
#include <cstdint>
#include <queue>
#include <memory>
#include <unordered_map>

#include "mongo/executor/async_stream_factory_interface.h"
#include "mongo/executor/async_stream_interface.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/rpc/protocol.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/mutex.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace executor {

class AsyncStreamInterface;

/**
 * A factory that produces mock streams to allow for testing of NetworkInterfaceASIO.
 *
 * The streams produced by this factory simulate a flow of Events (ConnectEvent,
 * ReadEvent, WriteEvent). The streams created by this factory will automatically
 * pause themselves at each Event, and the caller must unblock them by destroying
 * the Event object to continue.
 *
 * Example use of this factory:
 *
 *     AsyncMockStreamFactory factory();
 *
 *     // NIA will then call makeStream(...) to create new streams from the
 *     // factory, or the caller can do this manually.
 *
 *     // Wait for the desired stream to exist
 *     auto stream = streamFactory.blockUntilStreamExists(host);
 *
 *     // If we do not care to inspect after a certain event, we can skip it:
 *     ConnectEvent{stream}.skip();
 *
 *     // To examine the stream at an Event, instantiate the event object.
 *     // When the Event object goes out of scope the stream will unblock.
 *     {
 *         WriteEvent write{stream};
 *
 *         // Inspect what NIA wrote to this stream:
 *         auto messageData = stream->popWrite();
 *         ...
 *     }
 *
 *     // The Event object will keep the stream blocked as long as it exists.
 *     // Use this window to perform operations on the stream or inspect it.
 *     {
 *         ReadEvent read{stream};
 *
 *         // Simulate data sent to this stream over the network
 *         stream->pushRead( ... );
 *
 *         // Or, simulate a networking error
 *         stream->setError( error_code );
 *     }
 */
class AsyncMockStreamFactory final : public AsyncStreamFactoryInterface {
public:
    AsyncMockStreamFactory() = default;

    std::unique_ptr<AsyncStreamInterface> makeStream(asio::io_service::strand* strand,
                                                     const HostAndPort& host) override;

    /**
     * A mock stream class for testing the egress networking layer.
     *
     * At the core of this class is an idea of deferring actions and allowing inspection
     * of state of the stream before those actions happen.
     *
     * This class operates on the assumption that two threads are in use: a networking
     * thread used by NIA to issue IO calls on the MockStream, and a test thread to
     * wait on those calls and react.
     *
     * When the test thread creates an Event object, the constructor sends it to wait
     * on a condition variable. When NIA issues an IO call on the stream, the MockStream
     * load the proper handler into a placeholder, and then calls notify() on the
     * condition variable. At that point the stream is paused and the test thread
     * may operate on it.
     */
    class MockStream final : public AsyncStreamInterface {
    public:
        MockStream(asio::io_service::strand* strand,
                   AsyncMockStreamFactory* factory,
                   const HostAndPort& target);

        // Use unscoped enum so we can specialize on it
        enum StreamState {
            kRunning,
            kBlockedBeforeConnect,
            kBlockedBeforeRead,
            kBlockedAfterWrite,
            kCanceled,
        };

        ~MockStream();

        void connect(asio::ip::tcp::resolver::iterator endpoints,
                     ConnectHandler&& connectHandler) override;
        void write(asio::const_buffer buf, StreamHandler&& writeHandler) override;
        void read(asio::mutable_buffer buf, StreamHandler&& readHandler) override;

        bool isOpen() override;

        HostAndPort target();

        StreamState waitUntilBlocked();

        void cancel() override;

        std::vector<uint8_t> popWrite();
        void pushRead(std::vector<uint8_t> toRead);

        void setError(std::error_code ec);

        void unblock();

        void simulateServer(
            rpc::Protocol proto,
            const stdx::function<RemoteCommandResponse(RemoteCommandRequest)> replyFunc);

    private:
        using Action = stdx::function<void()>;

        void _defer(StreamState state, Action&& handler);
        void _defer_inlock(StreamState state, Action&& handler);
        void _unblock_inlock();

        asio::io_service::strand* _strand;

        AsyncMockStreamFactory* _factory;
        HostAndPort _target;

        stdx::mutex _mutex;

        stdx::condition_variable _deferredCV;
        StreamState _state{kRunning};

        std::queue<std::vector<uint8_t>> _readQueue;
        std::queue<std::vector<uint8_t>> _writeQueue;

        std::error_code _error;

        Action _deferredAction;
    };

    MockStream* blockUntilStreamExists(const HostAndPort& host);

private:
    void _createStream(const HostAndPort& host, MockStream* stream);
    void _destroyStream(const HostAndPort& host);

    stdx::mutex _factoryMutex;
    stdx::condition_variable _factoryCv;

    std::unordered_map<HostAndPort, MockStream*> _streams;
};

template <int EventType>
class StreamEvent {
public:
    StreamEvent(AsyncMockStreamFactory::MockStream* stream) : _stream(stream) {
        ASSERT(stream->waitUntilBlocked() == EventType);
    }

    void skip() {
        _stream->unblock();
        skipped = true;
    }

    ~StreamEvent() {
        if (!skipped) {
            skip();
        }
    }

private:
    bool skipped = false;
    AsyncMockStreamFactory::MockStream* _stream = nullptr;
};

using ReadEvent = StreamEvent<AsyncMockStreamFactory::MockStream::StreamState::kBlockedBeforeRead>;
using WriteEvent = StreamEvent<AsyncMockStreamFactory::MockStream::StreamState::kBlockedAfterWrite>;
using ConnectEvent =
    StreamEvent<AsyncMockStreamFactory::MockStream::StreamState::kBlockedBeforeConnect>;

}  // namespace executor
}  // namespace mongo
