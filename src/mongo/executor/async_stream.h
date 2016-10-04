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

#include "mongo/executor/async_stream_interface.h"

namespace mongo {
namespace executor {

class AsyncStream final : public AsyncStreamInterface {
public:
    AsyncStream(asio::io_service::strand* strand);

    ~AsyncStream();

    void connect(asio::ip::tcp::resolver::iterator iter, ConnectHandler&& connectHandler) override;

    void write(asio::const_buffer buffer, StreamHandler&& streamHandler) override;

    void read(asio::mutable_buffer buffer, StreamHandler&& streamHandler) override;

    void cancel() override;

    bool isOpen() override;

private:
    asio::io_service::strand* const _strand;
    asio::ip::tcp::socket _stream;
    bool _connected = false;
};

}  // namespace executor
}  // namespace mongo
