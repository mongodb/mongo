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

#include "mongo/executor/async_mock_stream_factory.h"

#include <exception>
#include <iterator>
#include <system_error>

#include "mongo/base/system_error.h"
#include "mongo/rpc/command_reply_builder.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/legacy_reply_builder.h"
#include "mongo/rpc/request_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"

namespace mongo {
namespace executor {

namespace {
StringData stateToString(AsyncMockStreamFactory::MockStream::StreamState state) {
    switch (state) {
        case AsyncMockStreamFactory::MockStream::StreamState::kRunning:
            return "Running";
        case AsyncMockStreamFactory::MockStream::StreamState::kBlockedBeforeConnect:
            return "Blocked before connect";
        case AsyncMockStreamFactory::MockStream::StreamState::kBlockedAfterWrite:
            return "Blocked after write";
        case AsyncMockStreamFactory::MockStream::StreamState::kBlockedBeforeRead:
            return "Blocked before read";
        case AsyncMockStreamFactory::MockStream::StreamState::kCanceled:
            return "Canceled";
    }
    MONGO_UNREACHABLE;
}

template <typename Handler>
void checkCanceled(asio::io_service::strand* strand,
                   AsyncMockStreamFactory::MockStream::StreamState* state,
                   Handler&& handler,
                   std::size_t bytes,
                   std::error_code ec = std::error_code()) {
    auto wasCancelled = (*state == AsyncMockStreamFactory::MockStream::StreamState::kCanceled);
    *state = AsyncMockStreamFactory::MockStream::StreamState::kRunning;
    strand->post([handler, wasCancelled, bytes, ec] {
        handler(wasCancelled ? make_error_code(asio::error::operation_aborted) : ec, bytes);
    });
}

}  // namespace

std::unique_ptr<AsyncStreamInterface> AsyncMockStreamFactory::makeStream(
    asio::io_service::strand* strand, const HostAndPort& target) {
    return stdx::make_unique<MockStream>(strand, this, target);
}

void AsyncMockStreamFactory::_createStream(const HostAndPort& host, MockStream* stream) {
    stdx::lock_guard<stdx::mutex> lk(_factoryMutex);
    log() << "creating stream for: " << host;
    _streams.emplace(host, stream);
    _factoryCv.notify_all();
}

void AsyncMockStreamFactory::_destroyStream(const HostAndPort& host) {
    stdx::lock_guard<stdx::mutex> lk(_factoryMutex);
    log() << "destroying stream for: " << host;
    _streams.erase(host);
}

AsyncMockStreamFactory::MockStream* AsyncMockStreamFactory::blockUntilStreamExists(
    const HostAndPort& host) {
    stdx::unique_lock<stdx::mutex> lk(_factoryMutex);

    auto iter = std::begin(_streams);

    _factoryCv.wait(lk, [&] { return (iter = _streams.find(host)) != std::end(_streams); });

    return iter->second;
}

AsyncMockStreamFactory::MockStream::MockStream(asio::io_service::strand* strand,
                                               AsyncMockStreamFactory* factory,
                                               const HostAndPort& target)
    : _strand(strand), _factory(factory), _target(target) {
    _factory->_createStream(_target, this);
}

AsyncMockStreamFactory::MockStream::~MockStream() {
    _factory->_destroyStream(_target);
}

void AsyncMockStreamFactory::MockStream::connect(asio::ip::tcp::resolver::iterator endpoints,
                                                 ConnectHandler&& connectHandler) {
    // Suspend execution after "connecting"
    _defer(kBlockedBeforeConnect, [this, connectHandler, endpoints]() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        // We shim a lambda to give connectHandler the right signature since it doesn't take
        // a size_t param.
        checkCanceled(
            _strand,
            &_state,
            [connectHandler](std::error_code ec, std::size_t) { return connectHandler(ec); },
            0);
    });
}

void AsyncMockStreamFactory::MockStream::write(asio::const_buffer buf,
                                               StreamHandler&& writeHandler) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto begin = asio::buffer_cast<const uint8_t*>(buf);
    auto size = asio::buffer_size(buf);
    _writeQueue.push({begin, begin + size});

    // Suspend execution after data is written.
    _defer_inlock(kBlockedAfterWrite, [this, writeHandler, size]() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        checkCanceled(_strand, &_state, std::move(writeHandler), size);
    });
}

void AsyncMockStreamFactory::MockStream::cancel() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    log() << "cancel() for: " << _target;

    // If _state is kRunning then we don't have a deferred operation to cancel.
    if (_state == kRunning) {
        return;
    }

    _state = kCanceled;
}

void AsyncMockStreamFactory::MockStream::read(asio::mutable_buffer buf,
                                              StreamHandler&& readHandler) {
    // Suspend execution before data is read.
    _defer(kBlockedBeforeRead, [this, buf, readHandler]() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        int nToCopy = 0;

        // If we've set an error, return that instead of a read.
        if (!_error) {
            auto nextRead = std::move(_readQueue.front());
            _readQueue.pop();

            auto beginDst = asio::buffer_cast<uint8_t*>(buf);
            nToCopy = std::min(nextRead.size(), asio::buffer_size(buf));

            auto endSrc = std::begin(nextRead);
            std::advance(endSrc, nToCopy);

            auto endDst = std::copy(std::begin(nextRead), endSrc, beginDst);
            invariant((endDst - beginDst) == static_cast<std::ptrdiff_t>(nToCopy));
            log() << "read " << nToCopy << " bytes, " << (nextRead.size() - nToCopy)
                  << " remaining in buffer";
        }

        auto handler = readHandler;

        // If we did not receive all the bytes, we should return an error
        if (static_cast<size_t>(nToCopy) < asio::buffer_size(buf)) {
            handler = [readHandler](std::error_code ec, size_t len) {
                // If we have an error here we've been canceled, and that takes precedence
                if (ec)
                    return readHandler(ec, len);

                // Call the original handler with an error
                readHandler(make_error_code(ErrorCodes::InvalidLength), len);
            };
        }

        checkCanceled(_strand, &_state, std::move(handler), nToCopy, _error);
        _error.clear();
    });
}

void AsyncMockStreamFactory::MockStream::pushRead(std::vector<uint8_t> toRead) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_state != kRunning && _state != kCanceled);
    _readQueue.emplace(std::move(toRead));
}

void AsyncMockStreamFactory::MockStream::setError(std::error_code ec) {
    _error = ec;
}

std::vector<uint8_t> AsyncMockStreamFactory::MockStream::popWrite() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_state != kRunning && _state != kCanceled);
    auto nextWrite = std::move(_writeQueue.front());
    _writeQueue.pop();
    return nextWrite;
}

void AsyncMockStreamFactory::MockStream::_defer(StreamState state, Action&& handler) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _defer_inlock(state, std::move(handler));
}

void AsyncMockStreamFactory::MockStream::_defer_inlock(StreamState state, Action&& handler) {
    invariant(_state == kRunning);
    _state = state;

    invariant(!_deferredAction);
    _deferredAction = std::move(handler);
    _deferredCV.notify_one();
}

void AsyncMockStreamFactory::MockStream::unblock() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _unblock_inlock();
}

void AsyncMockStreamFactory::MockStream::_unblock_inlock() {
    // Can be canceled here at which point we will call the handler with the CallbackCanceled
    // status when we invoke the _deferredAction.
    invariant(_state != kRunning);

    if (_state != kCanceled) {
        _state = kRunning;
    }
    // Post our deferred action to resume state machine execution
    invariant(_deferredAction);
    _strand->post(std::move(_deferredAction));
    _deferredAction = nullptr;
}

auto AsyncMockStreamFactory::MockStream::waitUntilBlocked() -> StreamState {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _deferredCV.wait(lk, [this]() { return _state != kRunning; });
    log() << "returning from waitUntilBlocked, state: " << stateToString(_state);
    return _state;
}

HostAndPort AsyncMockStreamFactory::MockStream::target() {
    return _target;
}

void AsyncMockStreamFactory::MockStream::simulateServer(
    rpc::Protocol proto,
    const stdx::function<RemoteCommandResponse(RemoteCommandRequest)> replyFunc) {
    std::exception_ptr ex;
    uint32_t messageId = 0;

    RemoteCommandResponse resp;
    {
        WriteEvent write{this};

        std::vector<uint8_t> messageData = popWrite();
        Message msg(SharedBuffer::allocate(messageData.size()));
        memcpy(msg.buf(), messageData.data(), messageData.size());

        auto parsedRequest = rpc::makeRequest(&msg);
        ASSERT(parsedRequest->getProtocol() == proto);

        RemoteCommandRequest rcr(target(), *parsedRequest);

        messageId = msg.header().getId();

        // So we can allow ASSERTs in replyFunc, we capture any exceptions, but rethrow
        // them later to prevent deadlock
        try {
            resp = replyFunc(std::move(rcr));
        } catch (...) {
            ex = std::current_exception();
        }
    }

    auto replyBuilder = rpc::makeReplyBuilder(proto);
    replyBuilder->setCommandReply(resp.data);
    replyBuilder->setMetadata(resp.metadata);

    auto replyMsg = replyBuilder->done();
    replyMsg.header().setResponseToMsgId(messageId);

    {
        // The first read will be for the header.
        ReadEvent read{this};
        auto hdrBytes = reinterpret_cast<const uint8_t*>(replyMsg.header().view2ptr());
        pushRead({hdrBytes, hdrBytes + sizeof(MSGHEADER::Value)});
    }

    {
        // The second read will be for the message data.
        ReadEvent read{this};
        auto dataBytes = reinterpret_cast<const uint8_t*>(replyMsg.buf());
        auto pastHeader = dataBytes;
        std::advance(pastHeader, sizeof(MSGHEADER::Value));
        pushRead({pastHeader, dataBytes + static_cast<std::size_t>(replyMsg.size())});
    }

    if (ex) {
        // Rethrow ASSERTS after the NIA completes it's Write-Read-Read sequence.
        std::rethrow_exception(ex);
    }
}

bool AsyncMockStreamFactory::MockStream::isOpen() {
    return true;
}

}  // namespace executor
}  // namespace mongo
