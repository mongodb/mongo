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

#include "mongo/executor/async_secure_stream.h"

#include "mongo/base/system_error.h"
#include "mongo/config.h"
#include "mongo/util/log.h"
#include "mongo/util/net/ssl_manager.h"

#ifdef MONGO_CONFIG_SSL

namespace mongo {
namespace executor {

AsyncSecureStream::AsyncSecureStream(asio::io_service* io_service, asio::ssl::context* sslContext)
    : _stream(*io_service, *sslContext) {}

void AsyncSecureStream::connect(const asio::ip::tcp::resolver::iterator endpoints,
                                ConnectHandler&& connectHandler) {
    // Stash the connectHandler as we won't be able to call it until we re-enter the state
    // machine.
    _userHandler = std::move(connectHandler);
    asio::async_connect(_stream.lowest_layer(),
                        std::move(endpoints),
                        [this](std::error_code ec, asio::ip::tcp::resolver::iterator iter) {
                            if (ec != ErrorCodes::OK) {
                                return _userHandler(ec);
                            }
                            return _handleConnect(ec, std::move(iter));
                        });
}

void AsyncSecureStream::write(asio::const_buffer buffer, StreamHandler&& streamHandler) {
    asio::async_write(_stream, asio::buffer(buffer), std::move(streamHandler));
}

void AsyncSecureStream::read(asio::mutable_buffer buffer, StreamHandler&& streamHandler) {
    asio::async_read(_stream, asio::buffer(buffer), std::move(streamHandler));
}

void AsyncSecureStream::_handleConnect(std::error_code ec, asio::ip::tcp::resolver::iterator iter) {
    _stream.async_handshake(decltype(_stream)::client,
                            [this, iter](std::error_code ec) {
                                if (ec != ErrorCodes::OK) {
                                    return _userHandler(ec);
                                }
                                return _handleHandshake(ec, iter->host_name());
                            });
}

void AsyncSecureStream::_handleHandshake(std::error_code ec, const std::string& hostName) {
    auto certStatus =
        getSSLManager()->parseAndValidatePeerCertificate(_stream.native_handle(), hostName);
    if (!certStatus.isOK()) {
        warning() << certStatus.getStatus();
    }
    _userHandler(make_error_code(certStatus.getStatus().code()));
}

}  // namespace executor
}  // namespace mongo

#endif
