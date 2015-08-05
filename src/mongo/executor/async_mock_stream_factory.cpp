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

#include "mongo/executor/async_mock_stream_factory.h"

#include <exception>
#include <iterator>
#include <system_error>

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

std::unique_ptr<AsyncStreamInterface> AsyncMockStreamFactory::makeStream(
    asio::io_service* io_service, const HostAndPort& target) {
    return stdx::make_unique<MockStream>(io_service, this, target);
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

AsyncMockStreamFactory::MockStream::MockStream(asio::io_service* io_service,
                                               AsyncMockStreamFactory* factory,
                                               const HostAndPort& target)
    : _io_service(io_service), _factory(factory), _target(target) {
    _factory->_createStream(_target, this);
}

AsyncMockStreamFactory::MockStream::~MockStream() {
    _factory->_destroyStream(_target);
}

void AsyncMockStreamFactory::MockStream::connect(asio::ip::tcp::resolver::iterator endpoints,
                                                 ConnectHandler&& connectHandler) {
    {
        stdx::unique_lock<stdx::mutex> lk(_mutex);

        // Block before returning from connect.
        _block_inlock(kBlockedBeforeConnect, &lk);
    }
    _io_service->post([connectHandler, endpoints] { connectHandler(std::error_code()); });
}

void AsyncMockStreamFactory::MockStream::write(asio::const_buffer buf,
                                               StreamHandler&& writeHandler) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto begin = asio::buffer_cast<const uint8_t*>(buf);
    auto size = asio::buffer_size(buf);
    _writeQueue.push({begin, begin + size});

    // Block after data is written.
    _block_inlock(kBlockedAfterWrite, &lk);

    lk.unlock();
    _io_service->post([writeHandler, size] { writeHandler(std::error_code(), size); });
}

void AsyncMockStreamFactory::MockStream::cancel() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    log() << "cancel() for: " << _target;

    // TODO: do we actually need to mock cancel?
    // We may want a cancel queue the same way we have read and write queues
}

void AsyncMockStreamFactory::MockStream::read(asio::mutable_buffer buf,
                                              StreamHandler&& readHandler) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    // Block before data is read.
    _block_inlock(kBlockedBeforeRead, &lk);

    auto nextRead = std::move(_readQueue.front());
    _readQueue.pop();

    auto beginDst = asio::buffer_cast<uint8_t*>(buf);
    auto nToCopy = std::min(nextRead.size(), asio::buffer_size(buf));

    auto endSrc = std::begin(nextRead);
    std::advance(endSrc, nToCopy);

    auto endDst = std::copy(std::begin(nextRead), endSrc, beginDst);
    invariant((endDst - beginDst) == static_cast<std::ptrdiff_t>(nToCopy));
    log() << "read " << nToCopy << " bytes, " << (nextRead.size() - nToCopy)
          << " remaining in buffer";

    lk.unlock();
    _io_service->post([readHandler, nToCopy] { readHandler(std::error_code(), nToCopy); });
}

void AsyncMockStreamFactory::MockStream::pushRead(std::vector<uint8_t> toRead) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_state != kRunning);
    _readQueue.emplace(std::move(toRead));
}

std::vector<uint8_t> AsyncMockStreamFactory::MockStream::popWrite() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    invariant(_state != kRunning);
    auto nextWrite = std::move(_writeQueue.front());
    _writeQueue.pop();
    return nextWrite;
}

void AsyncMockStreamFactory::MockStream::_block_inlock(StreamState state,
                                                       stdx::unique_lock<stdx::mutex>* lk) {
    invariant(_state == kRunning);
    _state = state;
    _cv.notify_one();
    _cv.wait(*lk, [this]() { return _state == kRunning; });
}

void AsyncMockStreamFactory::MockStream::unblock() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _unblock_inlock();
}

void AsyncMockStreamFactory::MockStream::_unblock_inlock() {
    invariant(_state != kRunning);
    _state = kRunning;
    _cv.notify_one();
}

auto AsyncMockStreamFactory::MockStream::waitUntilBlocked() -> StreamState {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    _cv.wait(lk, [this]() { return _state != kRunning; });
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
        Message msg(messageData.data(), false);

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
    replyBuilder->setMetadata(resp.metadata);
    replyBuilder->setCommandReply(resp.data);

    auto replyMsg = replyBuilder->done();
    replyMsg->header().setResponseTo(messageId);

    {
        // The first read will be for the header.
        ReadEvent read{this};
        auto hdrBytes = reinterpret_cast<const uint8_t*>(replyMsg->header().view2ptr());
        pushRead({hdrBytes, hdrBytes + sizeof(MSGHEADER::Value)});
    }

    {
        // The second read will be for the message data.
        ReadEvent read{this};
        auto dataBytes = reinterpret_cast<const uint8_t*>(replyMsg->buf());
        auto pastHeader = dataBytes;
        std::advance(pastHeader, sizeof(MSGHEADER::Value));
        pushRead({pastHeader, dataBytes + static_cast<std::size_t>(replyMsg->size())});
    }

    if (ex) {
        // Rethrow ASSERTS after the NIA completes it's Write-Read-Read sequence.
        std::rethrow_exception(ex);
    }
}

}  // namespace executor
}  // namespace mongo
