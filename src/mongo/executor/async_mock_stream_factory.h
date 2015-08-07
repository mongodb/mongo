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
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/net/hostandport.h"

namespace mongo {
namespace executor {

class AsyncStreamInterface;

class AsyncMockStreamFactory final : public AsyncStreamFactoryInterface {
public:
    AsyncMockStreamFactory() = default;

    std::unique_ptr<AsyncStreamInterface> makeStream(asio::io_service* io_service,
                                                     const HostAndPort& host) override;

    class MockStream final : public AsyncStreamInterface {
    public:
        MockStream(asio::io_service* io_service,
                   AsyncMockStreamFactory* factory,
                   const HostAndPort& target);
        ~MockStream();

        void connect(asio::ip::tcp::resolver::iterator endpoints,
                     ConnectHandler&& connectHandler) override;
        void write(asio::const_buffer buf, StreamHandler&& writeHandler) override;
        void read(asio::mutable_buffer buf, StreamHandler&& readHandler) override;

        void waitUntilBlocked();

        std::vector<uint8_t> popWrite();
        void pushRead(std::vector<uint8_t> toRead);

        void unblock();

    private:
        void _unblock_inlock(stdx::unique_lock<stdx::mutex>* lk);
        void _block_inlock(stdx::unique_lock<stdx::mutex>* lk);

        asio::io_service* _io_service;

        AsyncMockStreamFactory* _factory;
        HostAndPort _target;


        stdx::mutex _mutex;

        stdx::condition_variable _cv;
        bool _blocked{false};

        std::queue<std::vector<uint8_t>> _readQueue;
        std::queue<std::vector<uint8_t>> _writeQueue;
    };

    MockStream* blockUntilStreamExists(const HostAndPort& host);

private:
    void _createStream(const HostAndPort& host, MockStream* stream);
    void _destroyStream(const HostAndPort& host);

    stdx::mutex _factoryMutex;
    stdx::condition_variable _factoryCv;

    std::unordered_map<HostAndPort, MockStream*> _streams;
};

}  // namespace executor
}  // namespace mongo
